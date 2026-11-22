/**
 *   Random Stroke - every insertion lands somewhere new, and never less than a
 *   minimum distance from the last one.
 *
 *   Ported from fray-d/OSSM-Lite (class RandomStroke) onto this fork's contract:
 *   millimetres, and a position relative to the near end of the sweep. Upstream
 *   drew from rand()/RAND_MAX in step space; this uses Arduino's random() and
 *   speaks millimetres, so the pattern behaves the same but is host-testable.
 */

#ifndef RANDOMSTROKE_H
#define RANDOMSTROKE_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief Random insertion. Sensation sets the smallest allowed travel as a
  fraction of the sweep: 0.1 at -100, 0.5 neutral, 0.9 at +100. A target is
  drawn, and redrawn up to ten times if it is closer than that to the previous
  one.
*/
/**************************************************************************/
class RandomStroke : public Pattern
{
public:
    RandomStroke(const char *str) : Pattern(str) {}

    void setSensation(float sensation)
    {
        _minTravelRatio = constrain(sensation / 250.0f + 0.5f, 0.1f, 0.9f);
    }

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        _index = index;

        float peak = peakSpeedMmPerSecond();
        float minTravel = _stroke * _minTravelRatio;

        // Draw a target at least minTravel from the last one. Upstream gave up
        // after ten tries and took what it had; keep that, because a large minimum
        // can make a draw fail again and again.
        float target = _lastTarget;
        float travel = 0.0f;
        for (int attempt = 0; attempt < 10; attempt++)
        {
            target = _stroke * (random(0, 10000) / 10000.0f);
            travel = fabsf(target - _lastTarget);
            if (travel >= minTravel)
            {
                break;
            }
        }
        _lastTarget = target;

        // The time for this move follows its length, so speed stays the speed the
        // user asked for and a short hop is quick.
        float moveTime = fmaxf(0.005f, 1.5f * travel / fmaxf(0.005f, peak));

        _nextMove.stroke = constrain(target, 0.0f, _stroke);
        _nextMove.speed = peak;
        _nextMove.acceleration = 3.0f * peak / moveTime;
        _nextMove.skip = false;

        return _nextMove;
    }

protected:
    float _lastTarget = 0.0f;
    float _minTravelRatio = 0.5f;
};

#endif // RANDOMSTROKE_H
