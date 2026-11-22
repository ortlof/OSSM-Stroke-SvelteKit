/**
 *   Patterns of the StrokeEngine
 *   A library to create a variety of stroking motions with a stepper or servo motor on an ESP32.
 *   https://github.com/theelims/StrokeEngine
 *
 * Copyright (C) 2025 theelims <elims@gmx.net>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Ported from openlust/LUST-motion lib/Pattern/Pattern.h. The contract changed
 * with the port and it is worth stating plainly:
 *
 *   A pattern no longer knows the machine. It is handed the stroke LENGTH and
 *   the full-stroke TIME in seconds, and it answers with a position in
 *   **0 .. stroke, in millimetres**, relative to the near end of the sweep.
 *   StrokeEngine adds the depth offset, converts to steps and clips.
 *
 * The old contract had every pattern carry _depth and hand back an absolute step
 * position, so each pattern re-derived the machine envelope and each one could
 * get it wrong on its own.
 */

#ifndef PATTERN_H
#define PATTERN_H

#include <Arduino.h>
#include <math.h>
#include <PatternMath.h>
#include <string.h>

#ifndef STRING_LEN
#define STRING_LEN 64 // Bytes used to initialize char array. No path, topic, name, etc. should exceed this value
#endif

/**************************************************************************/
/*!
  @brief  struct to return all parameters a pattern commands for one stroke.
          Positions and speeds are real world units, not steps: the pattern does
          not know how the machine is geared.
*/
/**************************************************************************/
typedef struct
{
    float stroke;       //!< Target position 0..stroke, in mm, relative to the near end of the sweep
    float speed;        //!< Speed of a move in [mm/s]
    float acceleration; //!< Acceleration to get to speed or halt in [mm/s^2]
    bool skip;          //!< no valid stroke, skip this set and query again later --> allows pauses between strokes
} motionParameters_t;

/**************************************************************************/
/*!
  @class Pattern
  @brief  Base class to derive your pattern from. Offers a unified set of
          functions to store all the values it is handed, and one function to
          produce the next move. A pattern should be self-contained.
          The sanity check whether motion parameters are physically possible is
          done by the StrokeEngine, which clips, crops or adjusts them while
          still having a smooth appearance.
*/
/**************************************************************************/
class Pattern
{
public:
    //! Constructor
    /*!
      @param str String containing the name of a pattern
    */
    Pattern(const char *str) { strcpy(_name, str); }

    //! Set the time a normal stroke should take to complete
    /*!
      @param speed time of a full stroke in [sec]
    */
    virtual void setTimeOfStroke(float speed) { _timeOfStroke = speed; }

    //! Set the maximum stroke a pattern may have
    /*!
      @param stroke stroke distance in mm
    */
    virtual void setStroke(float stroke) { _stroke = stroke; }

    //! Sensation is an additional parameter a pattern can take to alter its behaviour
    /*!
      @param sensation Arbitrary value from -100 to 100, with 0 beeing neutral
    */
    virtual void setSensation(float sensation) { _sensation = sensation; }

    //! Where the slow phase of a pattern starts, as a percentage of the active stroke.
    /*!
      A second parameter beside sensation, for the patterns that have a fast part and
      a slow part. Those patterns used to derive the slow point from `sensation`, so
      the Sensation slider and the pattern fought over one value: moving the slow
      point moved the sensation with it. This parameter owns the slow point alone.

      Patterns without a slow phase ignore it. `usesSlowStart()` says which is which,
      so a client can hide the control instead of offering a slider that does nothing.

      @param percent 0..100, the share of the active stroke at which the slow part begins
    */
    virtual void setSlowStart(float percent) { _slowStartPercent = percent; }

    //! True when the pattern reads `setSlowStart()`. False by default.
    virtual bool usesSlowStart() const { return false; }

    //! Add a short pause after a move has completed and before the next move is commanded.
    /*!
      @param dwellMillis Pause time in milliseconds. Clamped to 0..500 ms.
      Local addition to the upstream base: the sweep parks at the reversal point
      instead of slamming straight into the next move.
    */
    virtual void setEndpointDwellMillis(int dwellMillis) {
        _endpointDwellMillis = constrain(dwellMillis, 0, 500);
        _endpointDwellIndex = 0xffffffffUL;
    }

    //! Retrives the name of a pattern
    /*!
      @return c_string containing the name of a pattern
    */
    char *getName() { return _name; }

    //! The peak speed of a 1/3-1/3-1/3 trapezoid covering the whole stroke in the
    //! half-stroke time the engine handed us. That is the speed the user asked
    //! for, so a pattern that wants "half speed" or "the maximum" multiplies this
    //! rather than guessing from a step count. The patterns ported from
    //! fray-d/OSSM-Lite carried their own `_speed` field for exactly this value.
    float peakSpeedMmPerSecond() const {
        return 1.5f * _stroke / fmaxf(0.005f, _timeOfStroke);
    }

    //! Calculate the position of the next stroke based on the various parameters
    /*!
      @param index index of a stroke. Increments with every new stroke.
      @param retract hint that the engine is bringing the carriage back. Kept for
             signature compatibility with upstream; StrokeEngine does not use it
             yet and every pattern may ignore it.
      @return Set of motion parameters like speed, acceleration & position
    */
    virtual motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        _index = index;
        return _nextMove;
    }

protected:
    // StrokeEngine sets these before the first nextTarget(). The initialisers are
    // belt and braces: an uninitialised _timeOfStroke is a divide by zero in every
    // pattern that derives its speed from it.
    float _stroke = 0.0f;
    float _timeOfStroke = 1.0f;
    float _sensation = 0.0f;
    // Percentage of the active stroke at which the slow part of a pattern starts.
    // 50 is the middle, which is where these patterns sat before the parameter
    // existed.
    float _slowStartPercent = 50.0f;
    const float _infinite = 1.0e30; // an approximation for infinite, for when maximum machine speed and acceleration are wanted
    int _index = -1;
    char _name[STRING_LEN];
    motionParameters_t _nextMove = {0.0f, 0.0f, 0.0f, false};
    unsigned long _startDelayMillis = 0;
    bool _delayRunning = false;
    int _delayInMillis = 0;
    int _endpointDwellMillis = 0;
    unsigned long _endpointDwellStartMillis = 0;
    unsigned long _endpointDwellIndex = 0xffffffffUL;

    /*!
      @brief Start a delay timer which can be polled by calling _isStillDelayed().
      Uses internally the millis()-function.
    */
    void _startDelay()
    {
        _startDelayMillis = millis();
        _delayRunning = true;
    }

    /*!
      @brief Update a delay timer which can be polled by calling _isStillDelayed().
      Uses internally the millis()-function.
      @param delayInMillis delay in milliseconds
    */
    void _updateDelay(int delayInMillis)
    {
        _delayInMillis = delayInMillis;
    }

    /*!
      @brief Poll the state of a internal timer to create pauses between strokes.
      Uses internally the millis()-function.
      @return True, if the timer is running, false if it is expired.
    */
    bool _isStillDelayed()
    {
        // No delay has been started yet, so nothing is pending. Without this the
        // test compares against _startDelayMillis == 0, i.e. against absolute
        // uptime, and a pattern would sit out its first pause after every boot
        // while millis() catches up. Upstream LUST-motion still has that bug.
        if (_delayRunning == false)
        {
            return false;
        }
        return ((unsigned long)(millis() - _startDelayMillis) < (unsigned long)_delayInMillis);
    }

    /*!
      @brief Dwell before issuing the next pattern move. StrokeEngine calls
             nextTarget() only after the previous move has completed, so this
             pauses exactly at the reversal point. The first move is not delayed.
      @return True while the pattern should skip and wait.
    */
    bool _applyEndpointDwell(unsigned int index)
    {
        if (_endpointDwellMillis <= 0 || index == 0)
        {
            _nextMove.skip = false;
            return false;
        }

        if (_endpointDwellIndex != index)
        {
            _endpointDwellIndex = index;
            _endpointDwellStartMillis = millis();
            _nextMove.skip = true;
            return true;
        }

        bool stillDwelling = ((unsigned long)(millis() - _endpointDwellStartMillis) < (unsigned long)_endpointDwellMillis);
        _nextMove.skip = stillDwelling;
        return stillDwelling;
    }
};

#endif // PATTERN_H
