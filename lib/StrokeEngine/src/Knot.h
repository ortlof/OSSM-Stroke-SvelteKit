/**
 *   Knot - a three part cycle: pull out, travel fast to a sensation-controlled
 *   point, then creep the last stretch so the stop lands short of full depth.
 *
 *   Local pattern. Ported to the millimetre / relative-position contract, and then
 *   re-derived: the phases are defined by how much of the cycle's TIME each one
 *   gets, not by fixed speed factors.
 *
 *   Why that matters. The old form set each phase's speed as a factor of
 *   `stroke / timeOfStroke`. The creep used 0.03, which works out at 2 % of the
 *   commanded speed, and the fast phases used 1.1, which is only 73 % of it. So
 *   the pattern never reached the speed it was asked for, and the creep dominated
 *   the cycle: measured on the machine at 20 mm/s, the creep phase alone took
 *   about 40 seconds for a 40 mm stroke. A slower command made the ratio worse,
 *   and the cycle time was unbounded.
 *
 *   Now a cycle takes a fixed number of stroke times and every phase is paced by
 *   the commanded speed. The creep is still much slower than the rest because it
 *   covers a small distance over a long share of the cycle - slow by construction
 *   rather than by a magic factor, and it scales.
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

// How long each phase gets, in half-strokes. A cycle is therefore
// 0.75 + 0.75 + 2.5 = 4 half-strokes = 2 commanded stroke times, whatever the
// command: the creep covers a short distance over 2.5 of those, and that is what
// makes it slow. Keep the creep share clearly the largest, or the pattern stops
// feeling like a knot.
#ifndef KNOT_RETURN_TIME_SHARES
#define KNOT_RETURN_TIME_SHARES 0.75f
#endif

#ifndef KNOT_APPROACH_TIME_SHARES
#define KNOT_APPROACH_TIME_SHARES 0.75f
#endif

#ifndef KNOT_CREEP_TIME_SHARES
#define KNOT_CREEP_TIME_SHARES 2.5f
#endif

#ifndef KNOT_STOP_SHORT_RATIO
#define KNOT_STOP_SHORT_RATIO 0.92f
#endif

#ifndef KNOT_MIN_SLOW_RATIO_GAP
#define KNOT_MIN_SLOW_RATIO_GAP 0.05f
#endif

// A zero-length or zero-speed phase is a hang, not a move: the driver stays idle
// and StrokeEngine asks for the same move forever. Keep every phase off the floor.
#ifndef KNOT_MIN_SPEED_MM_PER_SECOND
#define KNOT_MIN_SPEED_MM_PER_SECOND 0.1f
#endif

#ifndef KNOT_MIN_ACCEL_MM_PER_SECOND_SQUARED
#define KNOT_MIN_ACCEL_MM_PER_SECOND_SQUARED 1.0f
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

        // -100 => early slow point, +100 => late slow point. Keep a guaranteed
        // slow section before the stop-short target, otherwise high sensation
        // values can collapse the creep into nothing.
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

        float slowStart = calculateSlowStart();
        float stopShort = calculateStopShort();

        switch (index % 3)
        {
        case 0:
            // Back out to the near end, from wherever the creep stopped.
            setMove(0.0f, stopShort, KNOT_RETURN_TIME_SHARES);
            break;

        case 1:
            // Fast travel to the sensation-controlled slow point.
            setMove(slowStart, slowStart, KNOT_APPROACH_TIME_SHARES);
            break;

        case 2:
            // Creep the rest of the way: a short distance over the longest share
            // of the cycle.
            setMove(stopShort, stopShort - slowStart, KNOT_CREEP_TIME_SHARES);
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

    //! One phase covering `distance` millimetres in `timeShares` half-strokes.
    /*!
      For the 1/3-1/3-1/3 trapezoid the other patterns use, the peak speed over a
      distance d in a time t is 1.5 d / t and the acceleration is 4.5 d / t^2.
      Those are the relations SimpleStroke already derives from `stroke` and
      `timeOfStroke`, so a phase built this way is consistent with the rest of the
      library instead of being a second, private set of magic factors.
    */
    void setMove(float targetStroke, float distance, float timeShares)
    {
        float phaseTime = fmaxf(KNOT_MIN_TIME_OF_STROKE, _timeOfStroke * timeShares);
        float span = fmaxf(0.0f, distance);

        _nextMove.stroke = constrain(targetStroke, 0.0f, _stroke);
        _nextMove.speed = fmaxf(KNOT_MIN_SPEED_MM_PER_SECOND,
                                1.5f * span / phaseTime);
        _nextMove.acceleration = fmaxf(KNOT_MIN_ACCEL_MM_PER_SECOND_SQUARED,
                                       4.5f * span / (phaseTime * phaseTime));
        _nextMove.skip = false;
    }
};

#endif // KNOT_H
