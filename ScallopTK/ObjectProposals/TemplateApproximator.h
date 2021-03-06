//------------------------------------------------------------------------------
// Title: TemplateApproximator.h
// Author: Matthew Dawkins
// Description: Double Donut Detection
//------------------------------------------------------------------------------

#ifndef SCALLOP_TK_TEMPLATE_APPROXIMATOR_H_
#define SCALLOP_TK_TEMPLATE_APPROXIMATOR_H_

//------------------------------------------------------------------------------
//                               Include Files
//------------------------------------------------------------------------------

//Standard C/C++
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <math.h>
#include <fstream>

//Opencv
#include <cv.h>
#include <cxcore.h>

//Scallop Includes
#include "ScallopTK/Utilities/Definitions.h"
#include "ScallopTK/Utilities/HelperFunctions.h"
#include "ScallopTK/ScaleDetection/ImageProperties.h"
#include "ScallopTK/EdgeDetection/GaussianEdges.h"

//Benchmarking
#ifdef TEMPLATE_BENCHMARKING
  #include "ScallopTK/Utilities/Benchmarking.h"
#endif

//------------------------------------------------------------------------------
//                             Function Prototypes
//------------------------------------------------------------------------------

namespace ScallopTK
{

void findTemplateCandidates( GradientChain& grad, CandidatePtrVector& cds,
  ImageProperties& imgProp, IplImage* mask = NULL );

}

#endif
