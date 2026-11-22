/**
 *   Simple Stroke - a trapezoidal stroke profile, 1/3 acceleration, 1/3 coasting,
 *   1/3 deceleration. Sensation has no effect.
 *
 *   Ported to the millimetre / relative-position contract. Kept because our tree
 *   has it and upstream LUST-motion dropped it from its table.
 */

#ifndef SIMPLESTROKE_H
#define SIMPLESTROKE_H

#include <Pattern.h>

/**************************************************************************/
/*!
  @brief  Simple Stroke Pattern. It creates a trapezoidal stroke profile
  with 1/3 acceleration, 1/3 coasting, 1/3 deceleration. Sensation has
  no effect.
*/
/**************************************************************************/
class SimpleStroke : public Pattern
{
public:
    SimpleStroke(const char *str) : Pattern(str) {}

    void setTimeOfStroke(float speed = 0)
    {
        // In & Out have same time, so we need to divide by 2
        _timeOfStroke = 0.5 * speed;
    }

    motionParameters_t nextTarget(unsigned int index, bool retract = false)
    {
        if (_applyEndpointDwell(index))
        {
            _index = index;
            return _nextMove;
        }
        // maximum speed of the trapezoidal motion
        _nextMove.speed = 1.5 * _stroke / _timeOfStroke;

        // acceleration to meet the profile
        _nextMove.acceleration = 3.0 * _nextMove.speed / _timeOfStroke;

        // odd stroke is moving out (near end), even stroke is moving in (far end)
        _nextMove.stroke = (index % 2) ? 0.0f : _stroke;

        _index = index;
        return _nextMove;
    }
};

#endif // SIMPLESTROKE_H
