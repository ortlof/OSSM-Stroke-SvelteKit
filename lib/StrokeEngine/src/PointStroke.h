/**
 *   Point Stroke - park the carriage at one point inside the stroke, chosen by
 *   the sensation.
 *
 *   Ported from fray-d/OSSM-Lite (Software/lib/StrokeEngine/src/pattern.h,
 *   class PointStroke) onto this fork's contract: millimetres, and a position
 *   relative to the near end of the sweep. Upstream worked in steps and carried
 *   its own `_speed` field; peakSpeedMmPerSecond() is the same value here.
 *
 *   The target does not change with the index, so StrokeEngine drives to it once
 *   and then has nothing left to do. That is the point: it is a positioning
 *   pattern, not a stroking one.
 */

#ifndef POINTSTROKE_H
#define POINTSTROKE_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief Moves to a point as defined by the sensation.
    -100 => the near end of the sweep
       0 => the middle
     100 => the far end
*/
/**************************************************************************/
class PointStroke : public Pattern
{
public:
    PointStroke(const char *str) : Pattern(str) {}

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        _index = index;

        float ratio = (_sensation + 100.0f) / 200.0f;
        float peak = peakSpeedMmPerSecond();

        _nextMove.speed = peak;
        _nextMove.acceleration = 3.0f * peak / fmaxf(0.005f, _timeOfStroke);
        _nextMove.stroke = constrain(_stroke * ratio, 0.0f, _stroke);
        _nextMove.skip = false;

        return _nextMove;
    }
};

#endif // POINTSTROKE_H
