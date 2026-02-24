# Build libjpeg
Steps to install libjpeg.so.8 after cmake and nasm installation:
```bash
   git clone https://github.com/libjpeg-turbo/libjpeg-turbo.git
   mkdir final_libs
   cd final_libs/
   cmake -G"Unix Makefiles" -DWITH_JPEG8=1 -DCMAKE_BUILD_TYPE=Debug ../libjpeg-turbo/
   make
   sudo cp -a libjpeg.so* /usr/lib64/

# copy libjpeg.so* to /lib/x86_64-linux-gnu/ for DeepStream to find
    cp -a libjpeg.so* /lib/x86_64-linux-gnu/
```

## check if libjpeg.so.8 is installed
   ls -l /usr/lib64/libjpeg.so.8

    /lib/x86_64-linux-gnu/libjpeg.so

    ldd /opt/nvidia/deepstream/deepstream/lib/libnvds_batch_jpegenc.so
    ls /lib/x86_64-linux-gnu/ | grep libjpeg


    mv /lib/x86_64-linux-gnu/libjpeg.so /lib/x86_64-linux-gnu/temp_libjpeg.so
    mv /lib/x86_64-linux-gnu/libjpeg.so.8.2.2 /lib/x86_64-linux-gnu/temp_libjpeg.so.8.2.2


## gdb
```bash
gdb --args ./test-app
# show locals in gdb
(gdb) info locals
# show all variables in gdb
(gdb) info variables
# show a specific variable in gdb
(gdb) print variable_name
# set a breakpoint in gdb
(gdb) break function_name
# go out of the current function in gdb
(gdb) finish
# go out of the current function when segmentation fault occurs in gdb
(gdb) handle SIGSEGV nostop noprint pass
# add a breakpoint at a specific line in gdb
(gdb) break test.c:258
# go into function calls in gdb
(gdb) step
```

## Error
https://forums.developer.nvidia.com/t/deepstream-image-meta-test-segmentation-fault-core-dumped/311838