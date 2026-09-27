About flash img with maskrom mode:

First enter maskrom mode.

panther-x2:
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool ld
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool db rk356x_loader_v1.26.114.bin
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool rci
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool wl 0 panther-x2.img
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool rd

H28k
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool ld
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool db rk3528_spl_loader_v1.10.105.bin
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool rci
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool wl 0 h28k.img
LD_LIBRARY_PATH=$PWD/lib ./rkdeveloptool rd

