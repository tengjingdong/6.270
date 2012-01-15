#ifndef __NAVIGATION_H
#define __NAVIGATION_H

// Tunable parameters
#define NAV_ROT_KP              -5
#define NAV_ROT_KI              0
#define NAV_ROT_KD              0.2

#define NAV_DRV_KP              -0
#define NAV_DRV_KI              0
#define NAV_DRV_KD              0

#define NAV_FWD_GAIN            40

// Fastest allowed rotation (left/right setpoint delta)
#define NAV_MAX_ROT             110

// "Close-enough" angle and distance
// POS_EPS is a distance squared
#define NAV_POS_EPS             2.0
#define NAV_ANG_EPS             3.0

// Maximum angular error before switching from front drive
// to in-place pivot (degrees)
#define NAV_ANG_DRV_LMT         20.0

#define NAV_THREAD_PRIORITY     10

#define VPS_PER_CM              22.3972

/* High-level commands */

void turnToHeading(float t);

void turnToPoint(float x, float y);

void moveToPoint(float x, float y, float v);

/* Navigation status */

void getPosition(float *x, float *y, float *t);

int isMovementComplete(void);

void waitForMovement(void);


/* Initialization */

int nav_init(void);

int nav_start(void);

/* Internal */

int nav_loop(void);

void setTarget(float x, float y, float t, float v);

#endif