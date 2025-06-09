#!/usr/bin/env sh

sudo apt install -y libopenal-dev libglew-dev libglfw3-dev libsndfile1-dev libmpg123-dev build-essential

./premake5Linux --with-librw gmake2 --no-git-hash

cd build

make config=debug_linux-amd64-librw_gl3_glfw-oal -j$(nproc)
