/**
 *   The pattern table.
 *
 *   Base layout and the first nine patterns come from openlust/LUST-motion
 *   lib/Pattern/PatternLib.h, with our names kept so the web UI and the REST
 *   `pattern_name` field do not change under the user. The last six come from
 *   fray-d/OSSM-Lite (Software/lib/StrokeEngine/src/pattern.h).
 *
 * Copyright (C) 2025 theelims <elims@gmx.net>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef PATTERN_LIB_H
#define PATTERN_LIB_H

#include <Pattern.h>
#include <SimpleStroke.h>
#include <TeasingPounding.h>
#include <RoboStroke.h>
#include <HalfnHalf.h>
#include <Deeper.h>
#include <StopNGo.h>
#include <Insist.h>
#include <Knot.h>
#include <RandomDepth.h>
// from fray-d/OSSM-Lite:
#include <HalfnOut.h>
#include <ProgressiveStroke.h>
#include <KnotStroke.h>
#include <SlapStroke.h>
#include <RandomStroke.h>
#include <PointStroke.h>
// <-- include your new pattern class here!

extern "C"
{
    static SimpleStroke simpleStroke("Simple Stroke");
    static TeasingPounding teasingPounding("Teasing or Pounding");
    static RoboStroke roboStroke("Robo Stroke");
    static HalfnHalf halfnHalf("Half'n'Half");
    static Deeper deeper("Deeper");
    static StopNGo stopNGo("Stop'n'Go");
    static Insist insist("Insist");
    static Knot knot("Knot");
    static RandomDepth randomDepth("RandomDepth");
    static HalfnOut halfnOut("Half'n'Out");
    static ProgressiveStroke progressiveStroke("Progressive");
    static KnotStroke knotStroke("Knot Stroke");
    static SlapStroke slapStroke("Slap");
    static RandomStroke randomStroke("Random Stroke");
    static PointStroke pointStroke("Point");
    // <-- instantiate your new pattern class here!

    /**************************************************************************/
    /*
      Array holding all different patterns. Please include any custom pattern here.
    */
    /**************************************************************************/
    static Pattern *patternTable[] = {
        &simpleStroke,
        &teasingPounding,
        &roboStroke,
        &halfnHalf,
        &deeper,
        &stopNGo,
        &insist,
        &knot,
        &randomDepth,
        // Appended, so the indices the UI and any saved state already use keep
        // pointing at the same pattern.
        &halfnOut,
        &progressiveStroke,
        &knotStroke,
        &slapStroke,
        &randomStroke,
        &pointStroke
        // <-- insert your new pattern class here!
    };

    static const unsigned int patternTableSize = sizeof(patternTable) / sizeof(patternTable[0]);
}

#endif // PATTERN_LIB_H
