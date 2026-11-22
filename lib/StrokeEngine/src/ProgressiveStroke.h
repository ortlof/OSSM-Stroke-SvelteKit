/**
 *   Progressive Stroke - the sweep walks out in segments over a cycle and then
 *   walks back, so the carriage advances a little further on each stroke.
 *
 *   Ported from fray-d/OSSM-Lite (class ProgressiveStroke) onto this fork's
 *   contract: millimetres, and a position relative to the near end of the sweep.
 *   Upstream kept its own `_speed` field in steps/s and did the segment
 *   arithmetic in integer steps; the arithmetic here is in millimetres, which is
 *   the same shape without the truncation.
 */

#ifndef PROGRESSIVESTROKE_H
#define PROGRESSIVESTROKE_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief Sensation sets how many segments the cycle is divided into: 2 at -100,
  11 neutral, 32 at +100. The cycle walks from the far end of the sweep to the
  near end one segment at a time and then back out, so a low sensation gives long
  slow advances and a high one gives a fast vibration over the whole sweep.
*/
/**************************************************************************/
class ProgressiveStroke : public Pattern
{
public:
    ProgressiveStroke(const char *str) : Pattern(str) {}

    void setSensation(float sensation)
    {
        _sensation = sensation;
        // maps sensation to useful values [2,32] with 11 beeing neutral
        if (sensation < 0)
        {
            _countStrokesForRamp = map(sensation, -100, 0, 2, 11);
        }
        else
        {
            _countStrokesForRamp = map(sensation, 0, 100, 11, 32);
        }
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

        // A segment length from the stroke. Never zero: the count is at least 2,
        // and a zero segment would make every stroke land on the same place.
        int count = max(2, _countStrokesForRamp);
        float segment = _stroke / float(count);

        // The pattern recycles, so walk the cycle with a modulo. Every segment
        // takes two strokes (one out, one in), and the turn costs one index,
        // which is why the cycle is count * 2 - 1 long.
        int fullCycleIndex = (index / 2) % (count * 2 - 1);
        int position = fullCycleIndex % count;

        if (fullCycleIndex > count - 1)
        {
            // walking back out
            position = count - position;
            if (index % 2)
            {
                position -= 2;
            }
        }
        else
        {
            // walking in
            if (!(index % 2))
            {
                position += 1;
            }
        }

        // Position 0 is the far end of the sweep; the near end grows with it.
        // A position past the end is clamped by the engine, as upstream relied on.
        _nextMove.stroke = constrain(_stroke - segment * float(position), 0.0f, _stroke);
        _nextMove.speed = peak;
        _nextMove.acceleration = 3.0f * peak / fmaxf(0.005f, _timeOfStroke);
        _nextMove.skip = false;

        return _nextMove;
    }

protected:
    int _countStrokesForRamp = 2;
};

#endif // PROGRESSIVESTROKE_H
