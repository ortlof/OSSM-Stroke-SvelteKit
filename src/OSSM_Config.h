/*
    OSSM hardware configuration.

    In the original OSSM-Stroke firmware this file held the machine constants and
    the hardware version in EEPROM. In this project:

      - the machine and motor values live in StrokeSettingsService and are
        persisted as /config/strokeSettings.json on LittleFS. They can be edited
        from the browser and they survive a firmware update.
      - the defaults are compiled in through build flags in platformio.ini
        (OSSM_STEP_PER_MM, OSSM_MAX_SPEED_MM_S, ...) with a fallback in
        StrokeSettingsService.h.
      - the EEPROM block is gone. A stale EEPROM image from the old firmware is
        simply ignored, so flash a fresh device or erase the flash once:
            pio run -t erase && pio run -t upload
*/

#define HW_VERSION 22  // divide by 10 for the real hardware version
