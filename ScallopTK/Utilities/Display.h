//------------------------------------------------------------------------------
// Title: Display.h
// Author: Matthew Dawkins
//------------------------------------------------------------------------------

#ifndef SCALLOP_TK_DISPLAY_SYS_H
#define SCALLOP_TK_DISPLAY_SYS_H

// C/C++ Includes
#include <iostream>
#include <string>
#include <vector>
#include <fstream>

// OpenCV Includes
#include "cv.h"
#include "cxcore.h"
#include "highgui.h"

// Scallop Includes
#include "ScallopTK/Utilities/Definitions.h"

// Namespaces
using namespace std;

//------------------------------------------------------------------------------
//                                  Variables
//------------------------------------------------------------------------------

const int DISPLAY_WIDTH = 435;
const int DISPLAY_HEIGHT = 345;

//------------------------------------------------------------------------------
//                              Function Definitions
//------------------------------------------------------------------------------

void initOutputDisplay();
void killOuputDisplay();
void displayImage( IplImage* img, string wname = DISPLAY_WINDOW_NAME );
void displayInterestPointImage( IplImage* img, CandidatePtrVector& cds );
void displayResultsImage( IplImage* img, CandidatePtrVector& Scallops );
void displayResultsImage( IplImage* img, DetectionPtrVector& cds, string Filename );

#endif
