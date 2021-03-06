
Scallop-TK
==========

The Scalable, Adaptive Localization, and Laplacian Object Proposal
(Scallop) Toolkit is a small computer vision toolkit aimed at
detecting roughly elliptical or blob-like objects in imagery. It
is brief port of some of my master's thesis code developed at RPI,
described further in the paper:

[Automatic Scallop Detection in Benthic Environments](Documentation/Paper.pdf)

Beyond what's in the paper, the toolkit has a few more modern optimizations
as well, such as the ability to run convolutional neural networks on top of the
described object proposal framework. It is useful as a general detector
for detecting any ellipsoidal objects, though it also contains specialized
subroutines targetting benthic organisms such as clams, scallops, urchins,
and others.

WARNING: This repository is still a little bit of a WIP because I haven't
touched the code in a few years and it was created when I was a student.
Code quality varies greatly file to file.


Core Detection Pipeline
-----------------------

![Pipeline Image](Documentation/ExamplePipeline.png)


Build Instructions
------------------

Requirements: CMake, OpenCV, Caffe (Optional)

First, install [CMake](https://cmake.org/runningcmake/) and build or install
[OpenCV](http://opencv.org/) and [Caffe](http://caffe.berkeleyvision.org/).

Next, checkout this repository, run CMake on it, point CMake to the installed
dependencies, and then build using your compiler of choice.

Alternatively, ScallopTK can be built in [VIAME](https://github.com/Kitware/VIAME.git)
via enabling it in the build settings (set VIAME_ENABLE_CAFFE=ON and
VIAME_ENABLE_SCALLOP_TK=ON). This can be useful and easier since it also builds all
of the dependencies of caffe.

I also recommend installing CUDA >= 7.0 if you have an NVIDIA graphics card for
the computational speed-up, prior to building Caffe.

Run Instructions
----------------

A manual is [provided](Documentation/Manual.pdf), though it is very out of date and in need of
updating. The most basic way to run the core detector pipeline is to run:

./ScallopDetector PROCESS_DIR InputDirectoryWithImages OutputDetectionFile.txt

You can switch between AdaBoost, CNN, and Combo classifiers in the SYSTEM_SETTINGS file.
