/**
 *   Knot - a three part cycle: pull out, travel fast to a sensation-controlled
 *   point, then creep the last stretch so the stop lands short of full depth.
 *
 *   Local pattern, ported to the millimetre / relative-position contract. The old
 *   version derived absolute step targets from the engine's _depth and clamped
 *   itself against _maxSpeed; the engine does both now, so this only has to
 *   describe the shape of the stroke.
 */

#ifndef KNOT_H
#define KNOT_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief Knot pattern with sensation-controlled slow-start position.

  Sensation mapping:
    -100 => slow part starts at 5% of the active stroke
       0 => slow part starts at 50% of the active stroke
     100 => slow part starts at 95% of the active stroke

  The 5%/95% clamp keeps both the fast and slow portions visible. Without it,
  the pattern can legitimately command a zero-length fast or slow move, which
  looks like the machine has stopped even when the code is technically doing
  exactly what it was told.

  StrokeEngine passes seconds per full stroke to setTimeOfStroke(). This class
  stores the half-stroke time internally, matching the other patterns.
*/
/**************************************************************************/

#ifndef KNOT_MIN_TIME_OF_STROKE
#define KNOT_MIN_TIME_OF_STROKE 0.005f
#endif

#ifndef KNOT_FAST_SPEED_FACTOR
#define KNOT_FAST_SPEED_FACTOR 1.1f
#endif

#ifndef KNOT_SLOW_SPEED_FACTOR
#define KNOT_SLOW_SPEED_FACTOR 0.03f
#endif

#ifndef KNOT_FAST_ACCEL_FACTOR
#define KNOT_FAST_ACCEL_FACTOR 1.50f
#endif

#ifndef KNOT_SLOW_ACCEL_FACTOR
#define KNOT_SLOW_ACCEL_FACTOR 2.30f
#endif

#ifndef KNOT_RETURN_ACCEL_FACTOR
#define KNOT_RETURN_ACCEL_FACTOR 2.00f
#endif

#ifndef KNOT_STOP_SHORT_RATIO
#define KNOT_STOP_SHORT_RATIO 0.92f
#endif

#ifndef KNOT_MIN_SLOW_RATIO_GAP
#define KNOT_MIN_SLOW_RATIO_GAP 0.05f
#endif

// A speed of zero is not a move, it is a hang: the engine would see the driver
// idle and ask again forever. Keep every commanded move off the floor.
#ifndef KNOT_MIN_SPEED_MM_PER_SECOND
#define KNOT_MIN_SPEED_MM_PER_SECOND 0.1f
#endif

class Knot : public Pattern
{
public:
    Knot(const char *str) : Pattern(str) {}

    void setTimeOfStroke(float timeOfStroke = 1.0f)
    {
        // StrokeEngine gives this value as seconds per full stroke, not FPM.
        float safeFullStrokeTime = constrain(timeOfStroke, 0.01f, 120.0f);

        // Other patterns store the time for one half-stroke internally.
        _timeOfStroke = fmaxf(KNOT_MIN_TIME_OF_STROKE, 0.5f * safeFullStrokeTime);
    }

    void setSensation(float sensation)
    {
        _sensation = constrain(sensation, -100.0f, 100.0f);

        // -100 => early slow point, +100 => late slow point.
        // Keep a guaranteed slow section before the stop-short target, otherwise
        // high sensation values can collapse the slow move into nothing.
        float mappedRatio = (_sensation + 100.0f) / 200.0f;
        float maxSlowStartRatio = fmaxf(0.05f, _stopShortRatio - KNOT_MIN_SLOW_RATIO_GAP);
        _slowStartRatio = constrain(mappedRatio, 0.05f, maxSlowStartRatio);
    }

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        _index = index;

        switch (index % 3)
        {
        case 0:
            // Pull out to the start of the active stroke.
            setMove(0.0f, KNOT_FAST_SPEED_FACTOR, KNOT_RETURN_ACCEL_FACTOR);
            break;

        case 1:
            // Fast travel to the sensation-controlled slow point.
            setMove(calculateSlowStart(), KNOT_FAST_SPEED_FACTOR, KNOT_FAST_ACCEL_FACTOR);
            break;

        case 2:
            // Slow move, but stop short of full depth. The next step pulls out.
            setMove(calculateStopShort(), KNOT_SLOW_SPEED_FACTOR, KNOT_SLOW_ACCEL_FACTOR);
            break;
        }

        return _nextMove;
    }

private:
    float _slowStartRatio = 0.70f;
    float _stopShortRatio = constrain(KNOT_STOP_SHORT_RATIO, 0.10f, 0.99f);

    float calculateSlowStart() const
    {
        return constrain(_stroke * _slowStartRatio, 0.0f, _stroke);
    }

    float calculateStopShort() const
    {
        float slowStart = calculateSlowStart();

        // Keep the stop-short target after the slow-start target...
        float target = _stroke * _stopShortRatio;
        target = fmaxf(target, slowStart + (_stroke * 0.01f));

        // ...and genuinely short of full stroke when there is enough room.
        target = fminf(target, _stroke * 0.99f);

        return fmaxf(target, 0.0f);
    }

    void setMove(float targetStroke, float speedFactor, float accelFactor)
    {
        float safeTimeOfStroke = fmaxf(KNOT_MIN_TIME_OF_STROKE, _timeOfStroke);

        _nextMove.stroke = constrain(targetStroke, 0.0f, _stroke);
        _nextMove.speed = fmaxf(KNOT_MIN_SPEED_MM_PER_SECOND,
                                speedFactor * _stroke / safeTimeOfStroke);
        _nextMove.acceleration = accelFactor * _nextMove.speed / safeTimeOfStroke;
        _nextMove.skip = false;
    }
};

#endif // KNOT_H
