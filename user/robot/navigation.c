/*
 * Navigation / localization controller
 */
#include "navigation.h"
#include "platform.h"

#include <joyos.h>
#include <lib/motion.h>
#include <math.h>

// Tunable parameters
#define NAV_ROT_KP              -5
#define NAV_ROT_KI              0
#define NAV_ROT_KD              0

#define NAV_DRV_KP              -0
#define NAV_DRV_KI              0
#define NAV_DRV_KD              0

#define NAV_FWD_GAIN            00

// "Close-enough" angle and distance
// POS_EPS is a distance squared
#define NAV_POS_EPS             1.0
#define NAV_ANG_EPS             5.0

// Maximum angular error before switching from front drive
// to in-place pivot (degrees)
#define NAV_ANG_DRV_LMT         20.0

#define NAV_THREAD_PRIORITY     10

#define VPS_PER_CM              22.3972

// Navigation target
float target_x;
float target_y;
float target_t;
float target_v;

// Navigation position
float current_x;
float current_y;
float current_t;

// Drive setpoints
int16_t left_setpoint;
int16_t right_setpoint;

// Internal nav state
uint8_t nav_thread_id;
struct lock nav_data_lock;
struct lock nav_done_lock;

enum nav_state_t {ROTATE, DRIVE};
enum nav_state_t nav_state; 

uint32_t vps_last_update;

struct pid_controller rotate_pid;
struct pid_controller drive_pid;

float normalize_angle(float angle) {
    while (angle > 360) {
        angle -= 360;
    }
    
    while (angle <= 0) {
        angle += 360;
    }
    return angle;
}

float angle_difference(float a1, float a2) {
    float t = normalize_angle(a1 - a2);
    if (t > 180) {
        return 360 - t;
    } else {
        return t;
    }

}

/*
 * API calls
 */
 
/* High-level commands */

void turnToHeading(float t) {
    acquire(&nav_data_lock);
    target_x = current_x;
    target_y = current_y;
    
    target_t = normalize_angle(t);
    release(&nav_data_lock);
}

void turnToPoint(float x, float y) {
    acquire(&nav_data_lock);
    
    float t = atan2(y - current_y, x - current_x) * 180 / M_PI;
    turnToHeading(t);
    release(&nav_data_lock);
}

void moveToPoint(float x, float y, float v) {
    acquire(&nav_data_lock);
    target_x = x;
    target_y = y;
    target_t = atan2( y - current_y, x - current_x) * 180 / M_PI;
    target_v = v;
    release(&nav_data_lock);
}

void setTarget(float x, float y, float t, float v) {
    acquire(&nav_data_lock);
    target_x = x;
    target_y = y;
    target_t = normalize_angle(t);
    target_v = v;
    left_setpoint = v;
    right_setpoint = v;
    release(&nav_data_lock);
}

/* Navigation status */

void getPosition(float *x, float *y, float *t) {
    acquire(&nav_data_lock);
    *x = current_x;
    *y = current_y;
    *t = current_t;
    release(&nav_data_lock);
}

int isMovementComplete(void) {
    return !is_held(&nav_done_lock);
}

void waitForMovementComplete(void) {
    acquire(&nav_done_lock);
    release(&nav_done_lock);
}


/*
 * PID control functions
 */

float rotate_pid_input(void) {
    float temp_t = fmod(current_t - target_t, 360);
    if(temp_t > 180)
    {
        temp_t -= 360;
    }
    else if(temp_t < -180)
    {
        temp_t += 360;
    }
    return temp_t;
}

void rotate_pid_output(float output) {
    if (output > 100) {
        output = 100;
    } else if (output < -100) {
        output = -100;
    }
    
    left_setpoint += output;
    right_setpoint -= output;
}

float drive_pid_input(void) {
    uint16_t l_enc = encoder_read(L_ENCODER_PORT);
    
    uint16_t r_enc = encoder_read(R_ENCODER_PORT);
    
    printf("L/R encoders: %u / %u\n", l_enc, r_enc);
    
    
    
    return (float) (r_enc - l_enc);
}

void drive_pid_output(float output) {
    left_setpoint += output;
    right_setpoint -= output;
}

void vps_update(void) {
    current_x = objects[0].x * VPS_PER_CM;
    current_y = objects[0].y * VPS_PER_CM;
    current_t = normalize_angle(objects[0].theta);
    
    vps_last_update = position_microtime[0];
}



/*
 * Nav thread initialization and main loop
 */

int nav_init(void) {
    
    target_x = 0; target_y = 0; target_t = 0;
    
    init_lock(&nav_data_lock, "nav_data_lock");
    init_lock(&nav_done_lock, "nav_done_lock");
    
    init_pid(&rotate_pid, NAV_ROT_KP, NAV_ROT_KI, NAV_ROT_KD,
                rotate_pid_input, rotate_pid_output);
    rotate_pid.goal = 0;
    rotate_pid.enabled = 1;
    
    init_pid(&drive_pid, NAV_DRV_KP, NAV_DRV_KI, NAV_DRV_KD,
                drive_pid_input, drive_pid_output);
    drive_pid.goal = 0;
    drive_pid.enabled = 1;
    
    nav_state = ROTATE;
    
    vps_last_update = position_microtime[0];
    printf("Waiting for VPS fix...");
    uint32_t start_time = get_time_us();
    while (vps_last_update == position_microtime[0]) {  // Wait for VPS update
        copy_objects();  
        if (get_time_us() - start_time > 1000000) {     // Timeout after 1 sec
            break;
        }
    }
    
    if (vps_last_update != position_microtime[0]) {
        vps_update();
        gyro_set_degrees(current_t);
        printf("done.\n");
    } else {
        printf("timeout.\n");
    }

    
    return 0;
}

int nav_start(void) {
 
    nav_thread_id = create_thread(nav_loop, 
                STACK_DEFAULT, NAV_THREAD_PRIORITY, "nav_loop");
    return 0;
}

void update_position() {
    int16_t l_enc = encoder_read(L_ENCODER_PORT);
    int16_t r_enc = encoder_read(R_ENCODER_PORT);
    float enc_dist = (l_enc + r_enc) / (2.0 * TICKS_PER_CM);
    
    current_x += enc_dist * cos(current_t * M_PI / 180); // Use old heading
    current_y += enc_dist * sin(current_t * M_PI / 180);
    
    if (nav_state == DRIVE) {
        target_t = atan2((target_y-current_y),(target_x-current_x)) * 180 / M_PI;
    }
    
    printf("Encoders show movement of %.2f cm\n", enc_dist);
    
    // Check for new VPS fix
    copy_objects();
    if (position_microtime[0] != vps_last_update) {
        printf("New VPS fix: dt %u usec.\n", position_microtime[0] - vps_last_update);
        vps_update();
    }
    
    current_t = normalize_angle(gyro_get_degrees());
    
    printf("X: %.2f \tY: %.2f \tT: %.2f \t\t\n", current_x, current_y, current_t);
}

int nav_loop(void) {
    acquire(&nav_done_lock);
    while (1) {
        acquire(&nav_data_lock);
        if (!is_held(&nav_done_lock)) {
            acquire(&nav_done_lock);
        }
        
        printf("Target x: %.2f\tTarget y: %.2f\tTarget t: %.2f\n", target_x, target_y, target_t);
        
        left_setpoint = 0;
        right_setpoint = 0;
        
        // Update position estimate
        update_position();
        
        float dist = sqrt(square(current_x - target_x) + 
                    square(current_y - target_y));
                    
        if (dist <= NAV_POS_EPS) {
            // Done
            release(&nav_done_lock);
        }
        
        // Change states if necessary

        if (nav_state == ROTATE) {

            if (angle_difference(current_t, target_t) <= NAV_ANG_EPS) {
                nav_state = DRIVE;
            }
        
        } else if (nav_state == DRIVE) {
                        
            printf("Nav deviation: %.2f\n", angle_difference(current_t, target_t));
            
            if (angle_difference(current_t, target_t) >= NAV_ANG_DRV_LMT) {
                nav_state = ROTATE;
                printf("Nav angle deviation exceeded\n");
            }
            
        }
        
        // Execute
        
        if (nav_state == ROTATE) {
            update_pid(&rotate_pid);
            printf("Rotating\n");
            
        } else if (nav_state == DRIVE) {
            printf("Driving, %.2f cm to target\n", dist);
            
            float forward_vel = fmin(target_v, dist * NAV_FWD_GAIN);

    	    left_setpoint = forward_vel;
            right_setpoint = forward_vel;

            update_pid(&rotate_pid);
    	    //update_pid(&drive_pid);
        }
        
        release(&nav_data_lock);
        
        printf("L/R Setpoints: %i / %i\n", left_setpoint, right_setpoint);
        
        encoder_reset(L_ENCODER_PORT);
        encoder_reset(R_ENCODER_PORT);

        setLRMotors(left_setpoint, right_setpoint);
	
    }
    return 0;
}

