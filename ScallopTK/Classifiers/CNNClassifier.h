#ifndef SCALLOP_TK_CNN_CLASSIFIER_H_
#define SCALLOP_TK_CNN_CLASSIFIER_H_

//------------------------------------------------------------------------------
//                               Include Files
//------------------------------------------------------------------------------

//Standard C/C++
#include <vector>

//OpenCV
#include <cv.h>

//Caffe
#include <caffe/net.hpp>

//Scallop Includes
#include "ScallopTK/Utilities/Definitions.h"
#include "ScallopTK/Classifiers/Classifier.h"
#include "ScallopTK/Classifiers/AdaClassifier.h"

//------------------------------------------------------------------------------
//                              Class Definitions
//------------------------------------------------------------------------------

namespace ScallopTK
{

class CNNClassifier : public Classifier
{
public:

  CNNClassifier();
  ~CNNClassifier();

  // Load the classifier system from a file
  virtual bool loadClassifiers(
    const SystemParameters& sysParams,
    const ClassifierParameters& clsParams );

  // Classify candidates points according to internal classifier
  //
  // Image should contain the input image
  // Candidates the input candidates to score
  // Positive will contain any candidates with positive classifications
  virtual void classifyCandidates( cv::Mat image,
    CandidatePtrVector& candidates,
    CandidatePtrVector& positive );

  // Does this classifier require feature extraction?
  bool requiresFeatures()
    { return ( preClass != NULL ); }

  // Does this classifier have anything to do with scallop detection?
  bool detectsScallops()
    { return isScallopDirected; }

  // Extract training samples
  //
  // Image should contain the input image
  // Candidates the input candidates to match
  // GroundTruth the groundtruth candidates
  virtual void extractSamples( cv::Mat image,
    CandidatePtrVector& candidates,
    CandidatePtrVector& groundTruth );

  // Number of individual output classes this classifier has
  virtual int outputClassCount();

  // Get information about each bin that this classifier outputs
  virtual ClassifierIDLabel* const getLabel( int label );

private:

  typedef std::vector< ClassifierIDLabel > IDVector;
  typedef caffe::Net< float > CNN;
  typedef caffe::Caffe::Brew DeviceMode;

  // Main (initial) classifier applied to all candidates
  CNN* initialClfr;
  IDVector initialClfrLabels;

  // Optional suppression classifiers
  CNN* suppressionClfr;
  IDVector suppressionClfrLabels;

  // Is this system aimed at scallops or something entirely different?
  bool isScallopDirected;

  // Detection thresholds
  double initialThreshold;
  double secondThreshold;
  double trainingPercentKeep;

  // Adaboost preclassifier
  AdaClassifier* preClass;

  // Are we in training mode
  bool isTrainingMode;
  std::string outputFolder;

  // GPU device settings
  DeviceMode deviceMode;
  int deviceID;
  double deviceMem;

  // Helper functions
  void deallocCNNs();
  cv::Mat getCandidateChip( cv::Mat image,
    Candidate* cd, int width, int height );

  unsigned classifyCandidates( cv::Mat image,
    CandidatePtrVector& candidates,
    CandidatePtrVector& positive,
    CNN& classifier, double threshold );

};

}

#endif
