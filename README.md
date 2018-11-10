# Race Monitor

Prototype tool to time races for [diyrobocars.com](https://diyrobocars.com).  Not user friendly.  Also an
experiment to practice C and implement everything with minimal dependencies.

Only dependencies are Linux and SDL2.

# Running

- Checkout the repository
- Install GCC (only complier tested)
- Install SDL2 dev library (sudo apt install libsdl2-dev on Ubuntu 16.04)
- a camera that supports V4L2 and YUYV (possibly all webcams)
- Run "./qc" from the project dir which will build and run

<img src="screenshot.jpg" width="75%" alt="Screenshot">

The program should start fullscreen with the color image on the right and a
grayscale image on the left with the real-time finish line and background
finish line in the top left corner.  The percentage of changed pixels from the
background is shown below the real-time finish line.  The white rectangle on
the color image is the finish line area and should turn green when motion is
detected.

Once a something is detected crossing the finish line, the timer will start.
Currently tracks 3 laps, total time, and marks fastest lap.

# Keyboard Commands
- ctrl-n: reset race timer
- q: quit
- c: capture a single frame as BMP
- r: toggle continously recording frames as BMPs (haven't used much, might not work properly)
- f: theoretically toggle fullscreen, but kind of janky

# How It Works

- Grabs frames at 20fps
- Convert to grayscale
- Copy out finish line rectangle
- Run median filter on finish line (too slow for whole image on my machine)
- At startup, 60 frames are averaged to build up a background of the finish line
- Once the background is valid, percentage of different pixels is calculated
- Occasional mixes in new background frames, but unclear that it's useful.
  Particularly in well lit environments

# Known Issues
- Initially, the finish line was narrower and fast moving cars could pass
  through without detection.  Haven't tested much since expanding the finish
  line width.  The camera sets to 20fps which means the car has to change
  enough pixels within the finish line rectangle for 50ms.  I haven't put any
  effort into figuring out exactly how many pixels wide it should be.  Also
  depends on the distance from the camera.

- Smaller cars had problems sometimes with being detected.  Probably related to
  above.

- If limited/no buffer around the finish line area and the track boundary,
  poorly driving cars may "pass" the finish line outside the track.  Up to you
  if this is an actual problem.

# Unknown Issues
- Probably many related to limited C programming experience;)
- Better algorithms
- Better implementation of algorithms
