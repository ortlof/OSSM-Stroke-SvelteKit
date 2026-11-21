/*
    Pin Definitions - Drivers, homing and safety
    OSSM Reference board users are unlikely to need to modify this! See OSSM_Config.h
*/

/*
        Driver pins
*/

// Pin that pulses on servo/stepper steps - likely labelled PUL on drivers.
#define SERVO_PULSE       14
// Pin connected to driver/servo step direction - likely labelled DIR on drivers.
// N.b. to iHSV57 users - DIP switch #5 can be flipped to invert motor direction entirely
#define SERVO_DIR         27
// Enable Pin Motor Driver 
#define SERVO_ENABLE      26     

#define SERVO_ALM_PIN        39        // Servo ALM Output
#define SERVO_PED_PIN        36        // Servo PED Output
/*
    Homing and safety pins
*/
#define SERVO_ENDSTOP     12        // Homing Pin one Homing Switch IO pin to Ground
/*
    The cable remote of the original firmware is gone on purpose: no rotary
    encoder, no speed potentiometer, no OLED. All of it is in the browser now.
    The old pins were SPEED_POT_PIN 34, ENC_SW 35, ENC_CLK 18, ENC_DT 5,
    REMOTE_SDA 21, REMOTE_CLK 19. Do not reuse 34 and 35: they are input only.
*/
