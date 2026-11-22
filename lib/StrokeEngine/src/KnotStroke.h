/**
 *   Knot Stroke - a three part stroke: out to the start, in to a point the
 *   sensation picks, then the last stretch to full depth at half speed.
 *
 *   Ported from fray-d/OSSM-Lite (class KnotStroke) onto this fork's contract:
 *   millimetres, positions relative to the near end of the sweep. Upstream kept
 *   its own `_speed` field; peakSpeedMmPerSecond() is that value here.
 *
 *   This is a different idea from this fork's own Knot, which creeps the last
 *   stretch and deliberately stops SHORT of full depth. This one pushes slowly
 *   all the way to the end. Both are in the table; they are not the same pattern.
 */

#ifndef KNOTSTROKE_H
#define KNOTSTROKE_H

#include <Pattern.h>

// Keep the middle target clear of both ends of the sweep. Upstream let the
// sensation put it exactly on the far end, which leaves the final phase with no
// travel at all, and exactly on the near end, which does the same to the middle
// phase. Either way a third of the cycle becomes a no-op. The same 5 % guard this
// fork uses in its own Knot.
#ifndef KNOTSTROKE_MIN_FINAL_GAP
#define KNOTSTROKE_MIN_FINAL_GAP 0.05f
#endif

/**************************************************************************/
/*!
  @brief Knot stroke is a three part stroke.
  1. Out to the start of the active stroke, at three quarter speed.
  2. In to a depth determined by the sensation, at full speed.
  3. In to full depth at half speed. This is the knot.

  Sensation -100 puts the middle target at the near end of the sweep, 0 in the
  middle, +100 as close to the far end as the 5 % guard allows.
*/
/**************************************************************************/
class KnotStroke : public Pattern
{
public:
    KnotStroke(const char *str) : Pattern(str) {}

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        _index = index;

        float peak = peakSpeedMmPerSecond();
        float ratio = (_sensation + 100.0f) / 200.0f;

        // Keep the middle target clear of BOTH ends. At +100 upstream put it on the
        // far end, which leaves the third phase with no travel at all; at -100 it
        // sits on the near end, which does the same to the second phase. Either way
        // a third of the cycle becomes a no-op the machine skips through.
        ratio = constrain(ratio, KNOTSTROKE_MIN_FINAL_GAP, 1.0f - KNOTSTROKE_MIN_FINAL_GAP);

        switch (index % 3)
        {
        case 0:
            // Out to the start of the active stroke.
            _nextMove.speed = peak * 0.75f;
            _nextMove.stroke = 0.0f;
            break;

        case 1:
            // In to the point the sensation picks, at full speed.
            _nextMove.speed = peak;
            _nextMove.stroke = _stroke * ratio;
            break;

        default:
            // The last stretch, at half speed. This is the knot.
            _nextMove.speed = peak * 0.5f;
            _nextMove.stroke = _stroke;
            break;
        }

        _nextMove.acceleration = 3.0f * _nextMove.speed / fmaxf(0.005f, _timeOfStroke);
        _nextMove.skip = false;

        return _nextMove;
    }
};

#endif // KNOTSTROKE_H
