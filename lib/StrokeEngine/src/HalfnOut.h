/**
 *   Half'n'Out - every second cycle only pulls half way out, while the insertion
 *   is always full. Sensation changes the speed ratio between in and out.
 *
 *   Ported from fray-d/OSSM-Lite (class HalfnOut) onto this fork's contract:
 *   millimetres, positions relative to the near end of the sweep. Upstream kept
 *   its own `_speed` field in steps/s; peakSpeedMmPerSecond() is that value here.
 */

#ifndef HALFNOUT_H
#define HALFNOUT_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief  Like Teasing or Pounding, but every second stroke is only pulling half
  the way out. Sensation > 0 makes the in move faster (up to 4x) for a hard
  pounding sensation; values < 0 make the out move faster. The time for the
  overall stroke stays the same, even for the half ones.
*/
/**************************************************************************/
class HalfnOut : public Pattern
{
public:
    HalfnOut(const char *str) : Pattern(str) {}

    void setTimeOfStroke(float speed = 0)
    {
        // In & Out have same time, so we need to divide by 2
        _timeOfStroke = 0.5f * speed;
        _updateStrokeTiming();
    }

    void setStroke(float stroke)
    {
        _stroke = stroke;
        _updateStrokeTiming();
    }

    void setSensation(float sensation)
    {
        _sensation = sensation;
        _updateStrokeTiming();
    }

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        _index = index;

        // Every second cycle pulls only half way out. The insertion is always
        // full, so the near end of the sweep moves, not the far one.
        bool halfOut = ((index / 2) % 2) == 0;

        if (index % 2)
        {
            // odd stroke is moving out
            _nextMove.speed = 1.5f * _stroke / _timeOfOutStroke;
            _nextMove.stroke = halfOut ? 0.5f * _stroke : 0.0f;
            _nextMove.acceleration = 3.0f * _nextMove.speed / _timeOfOutStroke;
        }
        else
        {
            // even stroke is moving in
            _nextMove.speed = 1.5f * _stroke / _timeOfInStroke;
            _nextMove.stroke = _stroke;
            _nextMove.acceleration = 3.0f * _nextMove.speed / _timeOfInStroke;
        }
        _nextMove.skip = false;

        return _nextMove;
    }

protected:
    float _timeOfFastStroke = 1.0;
    float _timeOfInStroke = 1.0;
    float _timeOfOutStroke = 1.0;

    void _updateStrokeTiming()
    {
        if (_stroke <= 0.0f || _timeOfStroke <= 0.0f)
        {
            return;
        }
        float halfTime = _timeOfStroke;
        _timeOfFastStroke = halfTime * fscale(0.0f, 100.0f, 1.0f, 0.25f, abs(_sensation), 0.0f);
        _timeOfFastStroke = fmaxf(0.005f, _timeOfFastStroke);

        // positive sensation, in is faster
        if (_sensation > 0.0f)
        {
            _timeOfInStroke = _timeOfFastStroke;
            _timeOfOutStroke = fmaxf(0.005f, halfTime * 2.0f - _timeOfFastStroke);
        }
        // negative sensation, out is faster
        else
        {
            _timeOfOutStroke = _timeOfFastStroke;
            _timeOfInStroke = fmaxf(0.005f, halfTime * 2.0f - _timeOfFastStroke);
        }
    }
};

#endif // HALFNOUT_H
