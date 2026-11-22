/**
 *   Slap Stroke - a four part stroke: out to the start, back out to nine tenths,
 *   all the way in, then a pause.
 *
 *   Ported from fray-d/OSSM-Lite (class SlapStroke) onto this fork's contract.
 *   Upstream asked for the machine's maximum acceleration to get the slap; the
 *   base class's _infinite does the same job here, and the engine clips it.
 */

#ifndef SLAPSTROKE_H
#define SLAPSTROKE_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief Slap stroke is a four part stroke.
  1. Out to the start of the active stroke, at half speed.
  2. Back out to 90 % of the way in, as hard as the machine allows.
  3. All the way in, as hard as the machine allows.
  4. Pause, timed by the sensation: 100 ms at -100 up to 10 s at +100.
*/
/**************************************************************************/
class SlapStroke : public Pattern
{
public:
    SlapStroke(const char *str) : Pattern(str) {}

    void setSensation(float sensation)
    {
        _sensation = sensation;
        // maps sensation to a delay from 100ms to 10 sec
        _updateDelay(map(sensation, -100, 100, 100, 10000));
    }

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }

        // The delay is armed on the move and started on the next call, the way
        // StopNGo does it. The engine only asks for the next target once the
        // driver has stopped, so a timer started when the move is issued spends
        // most of its life while the machine is still travelling, and the pause
        // comes out as (delay - move time).
        if (_delayArmed)
        {
            _delayArmed = false;
            _startDelay();
        }

        float peak = peakSpeedMmPerSecond();
        float halfTime = fmaxf(0.005f, _timeOfStroke);

        _nextMove.speed = peak;
        _nextMove.acceleration = 3.0f * peak / halfTime;

        if (_isStillDelayed() == false)
        {
            switch (index % 3)
            {
            case 0:
                // Out to the start of the active stroke, at half speed. The pause
                // that follows belongs to this move.
                _nextMove.speed = peak * 0.5f;
                _nextMove.stroke = 0.0f;
                _nextMove.acceleration = 3.0f * _nextMove.speed / halfTime;
                _delayArmed = true;
                break;

            case 1:
                // Back out to nine tenths of the way in, hard.
                _nextMove.stroke = _stroke * 0.9f;
                _nextMove.acceleration = _infinite;
                break;

            default:
                // All the way in, hard. This is the slap.
                _nextMove.stroke = _stroke;
                _nextMove.acceleration = _infinite;
                break;
            }
            _nextMove.skip = false;
        }
        else
        {
            _nextMove.skip = true;
        }

        _index = index;
        return _nextMove;
    }

protected:
    bool _delayArmed = false;
};

#endif // SLAPSTROKE_H
