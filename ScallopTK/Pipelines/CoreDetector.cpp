//------------------------------------------------------------------------------
// CoreDetector.cpp
// author: Matt Dawkins
// description: Core detector pipeline of this mini toolkit
//------------------------------------------------------------------------------

#include "CoreDetector.h"

// Standard C/C++
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// OpenCV
#include <cv.h>
#include <cxcore.h>
#include <highgui.h>

// Internal Scallop Includes
#include "ScallopTK/Utilities/ConfigParsing.h"
#include "ScallopTK/Utilities/Display.h"
#include "ScallopTK/Utilities/Benchmarking.h"
#include "ScallopTK/Utilities/Threads.h"
#include "ScallopTK/Utilities/Filesystem.h"

#include "ScallopTK/ScaleDetection/ImageProperties.h"
#include "ScallopTK/ScaleDetection/StereoComputation.h"

#include "ScallopTK/EdgeDetection/WatershedEdges.h"
#include "ScallopTK/EdgeDetection/StableSearch.h"
#include "ScallopTK/EdgeDetection/ExpensiveSearch.h"

#include "ScallopTK/ObjectProposals/HistogramFiltering.h"
#include "ScallopTK/ObjectProposals/PriorStatistics.h"
#include "ScallopTK/ObjectProposals/AdaptiveThresholding.h"
#include "ScallopTK/ObjectProposals/TemplateApproximator.h"
#include "ScallopTK/ObjectProposals/CannyPoints.h"
#include "ScallopTK/ObjectProposals/Consolidator.h"

#include "ScallopTK/FeatureExtraction/ColorID.h"
#include "ScallopTK/FeatureExtraction/HoG.h"
#include "ScallopTK/FeatureExtraction/ShapeID.h"
#include "ScallopTK/FeatureExtraction/Gabor.h"

#include "ScallopTK/Classifiers/TrainingUtils.h"
#include "ScallopTK/Classifiers/Classifier.h"

namespace ScallopTK
{

// Number of worker threads
int THREADS;

// Variables for benchmarking tests
#ifdef ENABLE_BENCHMARKING
  const string BenchmarkingFilename = "BenchmarkingResults.dat";
  vector<double> executionTimes;
  ofstream benchmarkingOutput;
#endif

// Struct to hold inputs to the single image algorithm (1 per thread is created)
struct AlgorithmArgs {

  // ID for this thread
  int ThreadID;

  // Input image
  cv::Mat InputImage;

  // Input filename for input image, full path, if available
  string InputFilename;

  // Input filename for image, without directory
  string InputFilenameNoDir;

  // Output filename for Scallop List or Extracted Training Data
  string ListFilename;

  // Output filename for image result if enabled
  string OutputFilename;

  // Will have the algorithm use metadata if it is available
  bool UseMetadata;

  // Will only process the left half of the input image if set
  bool ProcessLeftHalfOnly;

  // Output options
  bool EnableOutputDisplay;
  bool EnableListOutput;
  bool OutputDuplicateClass;
  bool OutputProposalImages;
  bool OutputDetectionImages;

  // Search radii (used in meters if metadata is available, else pixels)
  float MinSearchRadiusMeters;
  float MaxSearchRadiusMeters;
  float MinSearchRadiusPixels;
  float MaxSearchRadiusPixels;

  // True if metadata is provided externally from a list
  bool MetadataProvided;
  float Pitch;
  float Roll;
  float Altitude;
  float FocalLength;

  // Container for color filters
  ColorClassifier *CC;

  // Container for external statistics collected so far (densities, etc)
  ThreadStatistics *Stats;

  // Container for loaded classifier system to use on this image
  Classifier *Model;

  // Did we last see a scallop or sand dollar cluster?
  bool ScallopMode;

  // Processing mode
  bool IsTrainingMode;

  // Training style (GUI or GT) if in training mode
  bool UseGTData;

  // GT Training keep factor
  float TrainingPercentKeep;

  // Process border interest points
  bool ProcessBorderPoints;

  // Pointer to GT input data if in training mode
  GTEntryList *GTData;

  // Output final detections
  DetectionVector FinalDetections;

  AlgorithmArgs()
  : Model( NULL ),
    GTData( NULL )
  {}
};

// Our Core Detection Algorithm - performs classification for a single image
//   inputs - shown above
//   outputs - returns NULL
void *processImage( void *InputArgs ) {

//--------------------Get Pointers to Main Inputs---------------------

  // Read input arguments (pthread requires void* as argument type)
  AlgorithmArgs *Options = (AlgorithmArgs*) InputArgs;
  ColorClassifier *CC = Options->CC;
  ThreadStatistics *Stats = Options->Stats;

#ifdef ENABLE_BENCHMARKING
  executionTimes.clear();
  startTimer();
#endif

  // Declare input image in assorted formats for later operations
  cv::Mat inputImgMat = Options->InputImage;

  if( inputImgMat.cols == 0 || inputImgMat.rows == 0 )
  {
    throw std::runtime_error( "Invalid input image" );
  }

  if( Options->ProcessLeftHalfOnly )
  {
    inputImgMat = inputImgMat(
      cv::Rect( 0, 0, inputImgMat.cols/2, inputImgMat.rows ) );
  }

//----------------------Calculate Object Size-------------------------

  // Declare Image Properties reader (for metadata read, size calc, etc)
  ImageProperties inputProp;

  if( Options->UseMetadata )
  {
    // Automatically loads metadata from input file if necessary
    if( !Options->MetadataProvided )
    {
      inputProp.calculateImageProperties( Options->InputFilename, inputImgMat.cols,
        inputImgMat.rows, Options->FocalLength );
    }
    else
    {
      inputProp.calculateImageProperties( inputImgMat.cols, inputImgMat.rows,
         Options->Altitude, Options->Pitch, Options->Roll, Options->FocalLength );
    }

    if( !inputProp.hasMetadata() )
    {
      cerr << "ERROR: Failure to read image metadata for file ";
      cerr << Options->InputFilenameNoDir << endl;
      threadExit();
      return NULL;
    }
  }
  else
  {
    inputProp.calculateImageProperties( inputImgMat.cols, inputImgMat.rows);
  }

  // Get the min and max Scallop size from combined image properties and input parameters
  float minRadPixels = ( Options->UseMetadata ? Options->MinSearchRadiusMeters
    : Options->MinSearchRadiusPixels ) / inputProp.getAvgPixelSizeMeters();
  float maxRadPixels = ( Options->UseMetadata ? Options->MaxSearchRadiusMeters
    : Options->MaxSearchRadiusPixels ) / inputProp.getAvgPixelSizeMeters();

  // Threshold size scanning range
  if( maxRadPixels < 1.0 )
  {
    cerr << "WARN: Scallop scanning size range is less than 1 pixel for image ";
    cerr << Options->InputFilenameNoDir << ", skipping." << endl;
    threadExit();
    return NULL;
  }

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

//-------------------------Format Base Images--------------------------

  // Resize image to maximum size required for all operations
  //  Stats->getMaxMinRequiredRad returns the maximum required image size
  //  in terms of how many pixels the min scallop radius should be. We only
  //  resize the image if this results in a downscale.
  float resizeFactor = MAX_PIXELS_FOR_MIN_RAD / minRadPixels;

  if( resizeFactor < RESIZE_FACTOR_REQUIRED ) {
    cv::Mat resizedImgMat;

    cv::resize( inputImgMat, resizedImgMat,
      cv::Size( (int)( resizeFactor*inputImgMat.cols ),
                (int)( resizeFactor*inputImgMat.rows ) ) );

    inputImgMat = resizedImgMat;
    minRadPixels = minRadPixels * resizeFactor;
    maxRadPixels = maxRadPixels * resizeFactor;

  } else {
    resizeFactor = 1.0f;
  }

  // The remaining code uses legacy OpenCV API (IplImage)
  IplImage inputImgIplWrapper = inputImgMat;
  IplImage *inputImg = &inputImgIplWrapper;

  // Create processed mask - records which pixels belong to what
  IplImage *mask = cvCreateImage( cvGetSize( inputImg ), IPL_DEPTH_8U, 1 );
  cvSet( mask, cvScalar( 255 ) );

  // Records how many detections of each classification category we have
  // within the current image
  int detections[TOTAL_DESIG];
  for( unsigned int i=0; i<TOTAL_DESIG; i++ )
  {
    detections[i] = 0;
  }

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Convert input image to other formats required for later operations
  //  - 32f = Std Floating Point, Range [0,1]
  //  - 8u = unsigned bytes, Range [0,255]
  //  - lab = CIELab color space
  //  - gs = Grayscale
  //  - rgb = sRGB (although beware OpenCV may load this as BGR in mem)
  IplImage *imgRGB32f = cvCreateImage( cvGetSize(inputImg), IPL_DEPTH_32F, inputImg->nChannels );
  IplImage *imgLab32f = cvCreateImage( cvGetSize(inputImg), IPL_DEPTH_32F, inputImg->nChannels );
  IplImage *imgGrey32f = cvCreateImage( cvGetSize(inputImg), IPL_DEPTH_32F, 1 );
  IplImage *imgGrey8u = cvCreateImage( cvGetSize(inputImg), IPL_DEPTH_8U, 1 );
  IplImage *imgRGB8u = cvCreateImage( cvGetSize(inputImg), IPL_DEPTH_8U, 3 );

  float scalingFactor = 1 / ( pow( 2.0f, inputImg->depth ) - 1 );
  cvConvertScale( inputImg, imgRGB32f, scalingFactor );
  cvCvtColor( imgRGB32f, imgLab32f, CV_RGB2Lab );
  cvCvtColor( imgRGB32f, imgGrey32f, CV_RGB2GRAY );
  cvScale( imgGrey32f, imgGrey8u, 255. );
  cvScale( imgRGB32f, imgRGB8u, 255. );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Perform color classifications on base image
  //   Puts results in hfResults struct
  //   Contains classification results for different organisms, and sal maps
  hfResults *color = CC->performColorClassification( imgRGB32f,
    minRadPixels, maxRadPixels );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Calculate all required image gradients for later operations
  GradientChain gradients = createGradientChain( imgLab32f, imgGrey32f,
    imgGrey8u, imgRGB8u, color, minRadPixels, maxRadPixels );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

//-----------------------Detect ROIs-----------------------------

  // Containers for initial interest points
  CandidatePtrVector cdsColorBlob;
  CandidatePtrVector cdsAdaptiveFilt;
  CandidatePtrVector cdsTemplateAprx;
  CandidatePtrVector cdsCannyEdge;

  // Perform Difference of Gaussian blob detection on our color classifications
  if( 1 || Stats->processed < 5 || !Options->Model->detectsScallops() )
  {
    detectSalientBlobs( color, cdsColorBlob ); //<-- Better for small # of images
  }
  else
  {
    detectColoredBlobs( color, cdsColorBlob ); //<-- Better for large # of images
  }

  filterCandidates( cdsColorBlob, minRadPixels, maxRadPixels, true );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Perform Adaptive Filtering
  performAdaptiveFiltering( color, cdsAdaptiveFilt, minRadPixels, false );
  filterCandidates( cdsAdaptiveFilt, minRadPixels, maxRadPixels, true );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Template Approx Candidate Detection
  findTemplateCandidates( gradients, cdsTemplateAprx, inputProp, mask );
  filterCandidates( cdsTemplateAprx, minRadPixels, maxRadPixels, true );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

  // Stable Canny Edge Candidates
  findCannyCandidates( gradients, cdsCannyEdge );
  filterCandidates( cdsCannyEdge, minRadPixels, maxRadPixels, true );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

//---------------------Consolidate ROIs--------------------------

  // Containers for sorted IPs
  CandidatePtrVector cdsAllUnordered;
  CandidateQueue cdsAllOrdered;

  // Consolidate interest points
  prioritizeCandidates( cdsColorBlob, cdsAdaptiveFilt, cdsTemplateAprx,
    cdsCannyEdge, cdsAllUnordered, cdsAllOrdered, Stats );

#ifdef ENABLE_BENCHMARKING
  executionTimes.push_back( getTimeSinceLastCall() );
#endif

//------------------GT Merging Procedure------------------------

  CandidatePtrVector GTDetections;

  if( Options->IsTrainingMode && Options->UseGTData )
  {
    // Mark which interest points are in this image
    for( int i = 0; i < Options->GTData->size(); i++ )
    {
      GTEntry& Pt = (*Options->GTData)[i];
      if( Pt.Name == Options->InputFilenameNoDir )
      {
        Candidate *cd1 = convertGTToCandidate( Pt, resizeFactor );
        GTDetections.push_back( cd1 );
      }
    }
  }

  if( !Options->ProcessBorderPoints )
  {
    removeBorderCandidates( cdsAllUnordered, imgRGB32f );
  }

  if( Options->EnableOutputDisplay )
  {
    displayInterestPointImage( imgRGB32f, cdsAllUnordered );
  }

  if( Options->OutputProposalImages )
  {
    saveCandidates( imgRGB32f, cdsAllUnordered,
      Options->OutputFilename + ".proposals.png" );
  }

//--------------------Extract Features---------------------------

  if( Options->Model->requiresFeatures() )
  {
    // Initializes Candidate stats used for classification
    initalizeCandidateStats( cdsAllUnordered, inputImg->height, inputImg->width );

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Identifies edges around each IP
    edgeSearch( gradients, color, imgLab32f, cdsAllUnordered, imgRGB32f );

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Creates an unoriented gs HoG descriptor around each IP
    HoGFeatureGenerator gsHoG( imgGrey32f, minRadPixels, maxRadPixels, 0 );
    gsHoG.Generate( cdsAllUnordered );

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Creates an unoriented sal HoG descriptor around each IP
    HoGFeatureGenerator salHoG( color->SaliencyMap, minRadPixels, maxRadPixels, 1 );
    salHoG.Generate( cdsAllUnordered );

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Calculates size based features around each IP
    float sizeAdj = ( Options->UseMetadata ? 1.0 : 0.0008 );
      // Above is a hack to make size features more comparable when we have/don't
      // have input metadata used to compute size info

    for( int i=0; i<cdsAllUnordered.size(); i++ ) {
      calculateSizeFeatures( cdsAllUnordered[i], inputProp, resizeFactor, sizeAdj );
    }

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Calculates color based features around each IP
    createColorQuadrants( imgGrey32f, cdsAllUnordered );
    for( int i=0; i<cdsAllUnordered.size(); i++ ) {
      calculateColorFeatures( imgRGB32f, color, cdsAllUnordered[i] );
    }

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif

    // Calculates gabor based features around each IP
    calculateGaborFeatures( imgGrey32f, cdsAllUnordered );

#ifdef ENABLE_BENCHMARKING
    executionTimes.push_back( getTimeSinceLastCall() );
#endif
  }

//----------------------Classify ROIs----------------------------

  CandidatePtrVector interestingCds;
  CandidatePtrVector likelyObjects;
  DetectionPtrVector objects;

  if( Options->IsTrainingMode && !Options->UseGTData )
  {
    // If in training mode, have user enter Candidate classifications
    if( !getDesignationsFromUser( cdsAllOrdered, imgRGB32f, mask, detections,
           minRadPixels, maxRadPixels, Options->InputFilenameNoDir ) )
    {
      trainingExitFlag = true;
    }
  }
  else if( Options->IsTrainingMode )
  {
    Options->Model->extractSamples( imgRGB8u, cdsAllUnordered, GTDetections );
  }
  else
  {
    // Classify candidates, returning ones with positive classifications
    Options->Model->classifyCandidates( imgRGB8u, cdsAllUnordered, interestingCds );

    // Calculate expensive edges around each interesting candidate point
    if( Options->Model->requiresFeatures() )
    {
      expensiveEdgeSearch( gradients, color, imgLab32f, imgRGB32f, interestingCds );
    }

    // Perform cleanup by removing interest points which are part of another interest point
    removeInsidePoints( interestingCds, likelyObjects );

    // Interpolate correct object categories
    objects = interpolateResults( likelyObjects, Options->Model, Options->InputFilename );

    // Display Detections
    if( Options->EnableOutputDisplay ) {
      getDisplayLock();
      displayResultsImage( imgRGB32f, objects, Options->InputFilenameNoDir );
      unlockDisplay();
    }
  }

//-----------------------Update Stats----------------------------

  // Update Detection variables and mask
  /*if( !Options->IsTrainingMode )
  {
    for( unsigned int i=0; i<objects.size(); i++ ) {
      Detection *cur = objects[i];
      if( cur->isBrownScallop ) {
        detections[SCALLOP_BROWN]++;
        updateMask( mask, cur->r, cur->c, cur->angle, cur->major, cur->minor, SCALLOP_BROWN );
      } else if( cur->isWhiteScallop ) {
        detections[SCALLOP_WHITE]++;
        updateMask( mask, cur->r, cur->c, cur->angle, cur->major, cur->minor, SCALLOP_WHITE );
      } else if( cur->isBuriedScallop ) {
        detections[SCALLOP_BURIED]++;
        updateMaskRing( mask, cur->r, cur->c, cur->angle,
          cur->major*0.8, cur->minor*0.8, cur->major, SCALLOP_BROWN );
      }
    }
  }
  else
  {
    // Quick hack: If we're not trying to detect scallops, reuse scallop histogram for better ip detections
    for( unsigned int i=0; i<objects.size(); i++ ) {
      Detection *cur = objects[i];
      updateMask( mask, cur->r, cur->c, cur->angle, cur->major, cur->minor, SCALLOP_BROWN );
    }
  }*/

  // Update color classifiers from mask and detections matrix
  //CC->Update( imgRGB32f, mask, detections );

  // Update statistics
  //Stats->Update( detections, inputProp.getImgHeightMeters() * inputProp.getImgWidthMeters() );

  // Output results to image files
  if( Options->OutputDetectionImages )
  {
    saveScallops( imgRGB32f, objects, Options->OutputFilename + ".detections.png" );
  }

  // Resize results to input resolution, and output to text file
  DetectionVector resizedObjects = convertVector( objects );

  if( resizeFactor != 1.0f && resizeFactor != 0.0f )
  {
    resizeDetections( resizedObjects, 1.0f / resizeFactor );
  }

  if( Options->EnableListOutput && !Options->IsTrainingMode )
  {
    if( !appendInfoToFile( resizedObjects, Options->ListFilename,
      Options->InputFilenameNoDir ) )
    {
      cerr << "CRITICAL ERROR: Could not write to output list!" << endl;
    }
  }

  // Copy final detections to class output
  Options->FinalDetections = resizedObjects;

//-------------------------Clean Up------------------------------

  // Deallocate memory used by thread
  deallocateCandidates( cdsAllUnordered );
  deallocateDetections( objects );
  deallocateGradientChain( gradients );
  hfDeallocResults( color );

  cvReleaseImage( &imgRGB32f );
  cvReleaseImage( &imgRGB8u );
  cvReleaseImage( &imgGrey32f );
  cvReleaseImage( &imgLab32f );
  cvReleaseImage( &imgGrey8u );
  cvReleaseImage( &mask );

  // Update thread status
  markThreadAsFinished( Options->ThreadID );
  threadExit();
  return NULL;
}

//--------------File system manager / algorithm caller------------------

int runCoreDetector( const SystemParameters& settings )
{
  // Retrieve some contents from input
  string inputDir = settings.InputDirectory;
  string inputFile = settings.InputFilename;
  string outputDir = settings.OutputDirectory;
  string outputFile = settings.OutputFilename;

  // Compile a vector of all files to process with related info
  vector<string> inputFilenames; // filenames (full or relative path)
  vector<string> inputClassifiers; // corresponding classifier keys
  vector<float> inputPitch; // input pitches, if used
  vector<float> inputAltitudes; // input altitudes, if used
  vector<float> inputRoll; // input rolls, if used
  vector<string> subdirsToCreate; // subdirs we may be going to create
  vector<string> outputFilenames; // corresponding output filenames if enabled

  // GT file contents if required
  string GTfilename = inputDir + inputFile;
  GTEntryList* GTs = NULL;

  // Process directory mode
  if( settings.IsInputDirectory || settings.IsTrainingMode )
  {
    // Get a list of all files and sub directories in the input dir
    listAllFile( inputDir, inputFilenames, subdirsToCreate );

    // Remove files that don't have an image extension (jpg/tif)
    cullNonImages( inputFilenames );

    // Initialize classifier array
    inputClassifiers.resize( inputFilenames.size(), settings.ClassifierToUse );
  }
  // If we're in process list mode
  else if( !settings.IsTrainingMode )
  {
    // Read input list
    string list_fn = inputDir + inputFile;
    ifstream input( list_fn.c_str() );

    // Check to make sure list is open
    if( !input )
    {
      cerr << endl << "CRITICAL ERROR: " << "Unable to open input list!" << endl;
      return 0;
    }

    // Iterate through list
    while( !input.eof() )
    {
      string inputFile, classifierKey;
      float alt, pitch, roll;

      // Metadata is in the input files
      if( settings.IsMetadataInImage )
      {
        input >> inputFile >> classifierKey;
        removeSpaces( inputFile );
        removeSpaces( classifierKey );
        if( inputFile.size() > 0 && classifierKey.size() > 0 )
        {
          inputFilenames.push_back( inputFile );
          inputClassifiers.push_back( classifierKey );
        }
      }
      // Metadata is in the list
      else
      {
        input >> inputFile >> alt >> pitch >> roll >> classifierKey;
        removeSpaces( inputFile );
        removeSpaces( classifierKey );
        if( inputFile.size() > 0 && classifierKey.size() > 0 )
        {
          inputFilenames.push_back( inputFile );
          inputClassifiers.push_back( classifierKey );
          inputAltitudes.push_back( alt );
          inputRoll.push_back( roll );
          inputPitch.push_back( pitch );
        }
      }
    }
    input.close();

    // Set input dir to 0 because the list should have the full or relative path
    inputDir = "";
  }
  // We're in list training mode
  else if( 0 /*settings.IsTrainingMode && !settings.IsInputDirectory*/ )
  {
    // Read input list
    string list_fn = inputDir + inputFile;
    ifstream input( list_fn.c_str() );

    // Check to make sure list is open
    if( !input )
    {
      cerr << endl << "CRITICAL ERROR: Unable to open input list!" << endl;
      return 0;
    }

    // Read location of GT annotations
    char buffer[2048];
    input.getline(buffer,2048);
    GTfilename = buffer;
    removeSpaces( GTfilename );

    // Iterate through list
    while( !input.eof() )
    {
      string inputFile, classifierKey;
      float alt, pitch, roll;

      // Metadata is in the input files
      if( settings.IsMetadataInImage )
      {
        input >> inputFile;
        removeSpaces( inputFile );
        if( inputFile.size() > 0  )
        {
          inputFilenames.push_back( inputFile );
          inputClassifiers.push_back( classifierKey );
        }
      }
      // Metadata is in the list
      else
      {
        input >> inputFile >> alt >> pitch >> roll;
        removeSpaces( inputFile );
        if( inputFile.size() > 0 )
        {
          inputFilenames.push_back( inputFile );
          inputClassifiers.push_back( classifierKey );
          inputAltitudes.push_back( alt );
          inputRoll.push_back( roll );
          inputPitch.push_back( pitch );
        }
      }
    }
    input.close();

    // Set input dir to 0 because the list should have the full or relative path
    inputDir = "";
  }

  // Read GTs file if necessary
  if( settings.IsTrainingMode && settings.UseFileForTraining )
  {
    // srand for random adjustments
    srand( time( NULL ) );

    // Load csv file
    GTs = new GTEntryList;
    cout << GTfilename << endl;
    parseGTFile( GTfilename, *GTs );
  }

  // Check to make sure image list is not empty
  if( inputFilenames.size() == 0 )
  {
    cerr << "\nERROR: Input invalid or contains no valid images." << std::endl;
    return 0;
  }

  // Copy the folder structure in the input dir to the output dir
  if( settings.OutputDetectionImages && !settings.IsTrainingMode )
  {
    copyDirTree( subdirsToCreate, inputDir, outputDir );
  }

  // Format the output name vector for each input image
  formatOutputNames( inputFilenames, outputFilenames, inputDir, outputDir );

  // Set global thread count
  THREADS = settings.NumThreads;

  // Create output list filename
  string listFilename = outputDir + outputFile;

  // Check to make sure we can open the output file (and flush contents)
  if( settings.OutputList ) {
    ofstream fout( listFilename.c_str() );
    if( !fout.is_open() ) {
      cout << "ERROR: Could not open output list for writing!" << std::endl;
      return false;
    }
    fout.close();
  }

#ifdef ENABLE_BENCHMARKING
  // Initialize Timing Statistics
  initializeTimer();
  benchmarkingOutput.open( BenchmarkingFilename.c_str() );

  if( !benchmarkingOutput.is_open() ) {
    cout << "ERROR: Could not write to benchmarking file!" << std::endl;
    return false;
  }
#endif

  // Storage map for loaded classifier styles
  map< string, Classifier* > classifiers;

  // Preload all required classifiers just in case there's a mistake
  // in a config file (so it doesn't die midstream)
  cout << "Loading Classifier Systems... ";
  for( int i = 0; i < inputClassifiers.size(); i++ )
  {
    // If classifier key does not exist in our list
    if( classifiers.find( inputClassifiers[i] ) == classifiers.end() )
    {
      // Load classifier config
      ClassifierParameters cparams;
      if( !parseClassifierConfig( inputClassifiers[i], settings, cparams ) )
      {
        return 0;
      }

      // Load classifier system based on config settings
      Classifier* LoadedSystem = loadClassifiers( settings, cparams );

      if( LoadedSystem == NULL )
      {
        cout << "ERROR: Unabled to load classifier " << inputClassifiers[i] << endl;
        return 0;
      }

      classifiers[ inputClassifiers[i] ] = LoadedSystem;
    }
  }
  cout << "FINISHED" << endl;

  // Load Statistics/Color filters
  cout << "Loading Colour Filters... ";
  AlgorithmArgs *inputArgs = new AlgorithmArgs[THREADS];

  for( int i=0; i < THREADS; i++ )
  {
    inputArgs[i].CC = new ColorClassifier;
    inputArgs[i].Stats = new ThreadStatistics;
    if( !inputArgs[i].CC->loadFilters( settings.RootColorDIR, DEFAULT_COLORBANK_EXT ) ) {
      cerr << "ERROR: Could not load colour filters!" << std::endl;
      return 0;
    }
  }
  cout << "FINISHED" << std::endl;

  // Configure algorithm input based on settings
  for( int i=0; i<THREADS; i++ )
  {
    // Set thread output options
    inputArgs[i].IsTrainingMode = settings.IsTrainingMode;
    inputArgs[i].UseGTData = settings.UseFileForTraining;
    inputArgs[i].TrainingPercentKeep = settings.TrainingPercentKeep;
    inputArgs[i].ProcessBorderPoints = settings.LookAtBorderPoints;
    inputArgs[i].GTData = GTs;
    inputArgs[i].EnableListOutput = settings.OutputList;
    inputArgs[i].OutputDuplicateClass = settings.OutputDuplicateClass;
    inputArgs[i].OutputProposalImages = settings.OutputProposalImages;
    inputArgs[i].OutputDetectionImages = settings.OutputDetectionImages;
    inputArgs[i].EnableOutputDisplay = settings.EnableOutputDisplay;
    inputArgs[i].ScallopMode = true;
    inputArgs[i].MetadataProvided = !settings.IsMetadataInImage && !settings.IsInputDirectory;
    inputArgs[i].ListFilename = listFilename;
    inputArgs[i].MinSearchRadiusMeters = settings.MinSearchRadiusMeters;
    inputArgs[i].MaxSearchRadiusMeters = settings.MaxSearchRadiusMeters;
    inputArgs[i].MinSearchRadiusPixels = settings.MinSearchRadiusPixels;
    inputArgs[i].MaxSearchRadiusPixels = settings.MaxSearchRadiusPixels;
    inputArgs[i].UseMetadata = settings.UseMetadata;
    inputArgs[i].ProcessLeftHalfOnly = settings.ProcessLeftHalfOnly;
  }

  // Initiate display window for output
  if( settings.EnableOutputDisplay )
  {
    initOutputDisplay();
  }

  // Initialize training mode if in gui mode
  if( settings.IsTrainingMode && !settings.UseFileForTraining ) {
    if( !initializeTrainingMode( outputDir, outputFile ) ) {
      cerr << "ERROR: Could not initiate training mode!" << std::endl;
    }
  }

  // Cycle through all input files
  cout << endl << "Processing Files: " << endl << endl;
  cout << "Directory: " << inputDir << endl << endl;

  // For every file...
  for( unsigned int i=0; i<inputFilenames.size(); i++ ) {

    // Always set the focal length
    inputArgs[0].FocalLength = settings.FocalLength;

    // Set classifier related settings
    inputArgs[0].Model = classifiers[ inputClassifiers[i] ];
    inputArgs[0].TrainingPercentKeep = settings.TrainingPercentKeep;
    inputArgs[0].ProcessBorderPoints = settings.LookAtBorderPoints;

    // Set file/dir arguments
    string Dir, filenameNoDir;
    splitPathAndFile( inputFilenames[i], Dir, filenameNoDir );
    cout << filenameNoDir << "..." << endl;
    inputArgs[0].InputFilename = inputFilenames[i];
    inputArgs[0].OutputFilename = outputFilenames[i];
    inputArgs[0].InputFilenameNoDir = filenameNoDir;

    // Set metadata if required
    if( !settings.IsMetadataInImage && !settings.IsInputDirectory && !settings.IsTrainingMode )
    {
      inputArgs[0].Altitude = inputAltitudes[i];
      inputArgs[0].Pitch = inputPitch[i];
      inputArgs[0].Roll = inputRoll[i];
    }

    // Load image from file
    cv::Mat image;
    image = imread( inputFilenames[i], CV_LOAD_IMAGE_COLOR );
    inputArgs[0].InputImage = image;

    // Execute processing
    processImage( inputArgs );

#ifdef ENABLE_BENCHMARKING
    // Output benchmarking results to file
    for( unsigned int i=0; i<executionTimes.size(); i++ )
      benchmarkingOutput << executionTimes[i] << " ";
    benchmarkingOutput << endl;
#endif

    // Checks if user entered EXIT command in training mode
    if( settings.IsTrainingMode && trainingExitFlag )
    {
      break;
    }
  }

  // Deallocate algorithm inputs
  for( int i=0; i < THREADS; i++ ) {
    delete inputArgs[i].Stats;
    delete inputArgs[i].CC;
  }
  delete[] inputArgs;

  // Deallocate loaded classifier systems
  map< string, Classifier* >::iterator p = classifiers.begin();
  while( p != classifiers.end() )
  {
    delete p->second;
    p++;
  }

  // Remove output display window
  if( settings.EnableOutputDisplay )
  {
    killOuputDisplay();
  }

#ifdef ENABLE_BENCHMARKING
  // Close benchmarking output file
  benchmarkingOutput.close();
#endif

  // Close gui-training mode
  if( settings.IsTrainingMode && !settings.UseFileForTraining )
  {
    exitTrainingMode();
  }

  // Deallocate GT info if in training mode
  if( GTs )
  {
    delete GTs;
  }

  return 0;
}

// Streaming class definition, for use by external libraries
class CoreDetector::Priv
{
public:

  explicit Priv( const SystemParameters& sets );
  ~Priv();

  Classifier* classifier;
  AlgorithmArgs *inputArgs;
  SystemParameters settings;
  unsigned counter;
};

CoreDetector::Priv::Priv( const SystemParameters& sets )
{
  counter = 0;

  // Retrieve some contents from input
  settings = sets;
  string outputDir = settings.OutputDirectory;
  string outputFile = settings.OutputFilename;
  string listFilename = outputDir + outputFile;
  THREADS = settings.NumThreads;

  // Check to make sure we can open the output file (and flush contents)
  if( settings.OutputList && !listFilename.empty() ) {
    ofstream fout( listFilename.c_str() );
    if( !fout.is_open() ) {
      throw std::runtime_error( "Could not open output list for writing" );
    }
    fout.close();
  }

#ifdef ENABLE_BENCHMARKING
  // Initialize Timing Statistics
  initializeTimer();
  benchmarkingOutput.open( BenchmarkingFilename.c_str() );

  if( !benchmarkingOutput.is_open() ) {
    throw std::runtime_error( "Could not write to benchmarking file" );
  }
#endif

  // Load classifier config
  cout << "Loading Classifier System";

  ClassifierParameters cparams;

  if( !parseClassifierConfig( settings.ClassifierToUse, settings, cparams ) ) {
    throw std::runtime_error( "Unabled to read config for "+ settings.ClassifierToUse );
  }

  // Load classifier system based on config settings
  classifier = loadClassifiers( settings, cparams );

  if( classifier == NULL ) {
    throw std::runtime_error( "Unabled to load classifier " + settings.ClassifierToUse );
  }

  // Load Statistics/Color filters
  cout << "Loading Colour Filters... ";
  inputArgs = new AlgorithmArgs[THREADS];

  for( int i=0; i < THREADS; i++ )
  {
    inputArgs[i].CC = new ColorClassifier;
    inputArgs[i].Stats = new ThreadStatistics;

    if( !inputArgs[i].CC->loadFilters( settings.RootColorDIR, DEFAULT_COLORBANK_EXT ) ) {
      throw std::runtime_error( "Could not load colour filters" );
    }
  }

  cout << "FINISHED" << std::endl;

  // Configure algorithm input based on settings
  for( int i=0; i<THREADS; i++ )
  {
    // Set thread output options
    inputArgs[i].IsTrainingMode = settings.IsTrainingMode;
    inputArgs[i].Model = classifier;
    inputArgs[i].UseGTData = settings.UseFileForTraining;
    inputArgs[i].TrainingPercentKeep = settings.TrainingPercentKeep;
    inputArgs[i].ProcessBorderPoints = settings.LookAtBorderPoints;
    inputArgs[i].GTData = NULL;
    inputArgs[i].EnableListOutput = settings.OutputList;
    inputArgs[i].OutputDuplicateClass = settings.OutputDuplicateClass;
    inputArgs[i].OutputProposalImages = settings.OutputProposalImages;
    inputArgs[i].OutputDetectionImages = settings.OutputDetectionImages;
    inputArgs[i].EnableOutputDisplay = settings.EnableOutputDisplay;
    inputArgs[i].ScallopMode = true;
    inputArgs[i].MetadataProvided = !settings.IsMetadataInImage && !settings.IsInputDirectory;
    inputArgs[i].ListFilename = listFilename;
    inputArgs[i].FocalLength = settings.FocalLength;
    inputArgs[i].MinSearchRadiusMeters = settings.MinSearchRadiusMeters;
    inputArgs[i].MaxSearchRadiusMeters = settings.MaxSearchRadiusMeters;
    inputArgs[i].MinSearchRadiusPixels = settings.MinSearchRadiusPixels;
    inputArgs[i].MaxSearchRadiusPixels = settings.MaxSearchRadiusPixels;
    inputArgs[i].UseMetadata = settings.UseMetadata;
    inputArgs[i].ProcessLeftHalfOnly = settings.ProcessLeftHalfOnly;
  }

  // Initiate display window for output
  if( settings.EnableOutputDisplay )
  {
    initOutputDisplay();
  }

  // Cycle through all input files
  cout << endl << "Ready to Process Files" << endl;
}

CoreDetector::Priv::~Priv()
{
  // Deallocate algorithm inputs
  for( int i=0; i < THREADS; i++ ) {
    delete inputArgs[i].Stats;
    delete inputArgs[i].CC;
  }

  delete[] inputArgs;

  // Deallocate loaded classifier systems
  if( classifier )
  {
    delete classifier;
  }

  // Remove output display window
  if( settings.EnableOutputDisplay )
  {
    killOuputDisplay();
  }

#ifdef ENABLE_BENCHMARKING
  // Close benchmarking output file
  benchmarkingOutput.close();
#endif
}

CoreDetector::CoreDetector( const std::string& configFile )
{
  SystemParameters settings;

  if( !parseSystemConfig( settings, configFile ) )
  {
    throw std::runtime_error( "Unable to read system parameters file" );
  }

  data = new Priv( settings );
}

CoreDetector::CoreDetector( const SystemParameters& settings )
{
  data = new Priv( settings );
}

CoreDetector::~CoreDetector()
{
  if( data )
  {
    delete data;
  }
}

std::vector< Detection >
CoreDetector::processFrame( const cv::Mat& image,
 float pitch, float roll, float altitude )
{
  data->counter++;
  std::string frameID = "streaming_frame_" + INT_2_STR( data->counter );

  cv::Mat corrected;
  cv::cvtColor( image, corrected, cv::COLOR_RGB2BGR );

  data->inputArgs[0].InputImage = corrected;
  data->inputArgs[0].InputFilename = frameID;
  data->inputArgs[0].OutputFilename = frameID;
  data->inputArgs[0].InputFilenameNoDir = frameID;

  if( pitch != 0.0f || roll != 0.0f || altitude != 0.0f )
  {
    data->inputArgs[0].MetadataProvided = true;
    data->inputArgs[0].Pitch = pitch;
    data->inputArgs[0].Roll = roll;
    data->inputArgs[0].Altitude = altitude;
  }
  else
  {
    data->inputArgs[0].MetadataProvided = false;
  }

  // Execute processing
  processImage( data->inputArgs );

#ifdef ENABLE_BENCHMARKING
  // Output benchmarking results to file
  for( unsigned int i=0; i<executionTimes.size(); i++ )
    benchmarkingOutput << executionTimes[i] << " ";
  benchmarkingOutput << endl;
#endif

   // Get output from input args
  return data->inputArgs->FinalDetections;
}

std::vector< Detection >
CoreDetector::processFrame( const cv::Mat& leftImage,
  const cv::Mat& rightImage, float pitch, float roll, float altitude )
{
  cv::Size leftSize = leftImage.size();
  cv::Size rightSize = rightImage.size();

  Mat merged( leftSize.height, leftSize.width + rightSize.width, leftImage.depth() );
  Mat left( merged, cv::Rect( 0, 0, leftSize.width, leftSize.height ) );
  leftImage.copyTo( left );
  Mat right( merged, cv::Rect( leftSize.width, 0, rightSize.width, rightSize.height ) );
  rightImage.copyTo( right );

  return processFrame( merged, pitch, roll, altitude );
}

std::vector< Detection >
CoreDetector::processFrame( std::string filename,
 float pitch, float roll, float altitude )
{
  cv::Mat image;
  image = imread( filename, CV_LOAD_IMAGE_COLOR );

  return processFrame( image, pitch, roll, altitude );
}

}
