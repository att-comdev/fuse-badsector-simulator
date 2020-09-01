## fuse-badsector-simulator

The fuse-badsector-simulator tool mirrors raw disk images via FUSE mounts and
intends to mimic the behavior of physical HDDs with regard to bad sectors and
reallocation of those sectors when they are overwritten.

The current workflow is as follows:

1. A raw disk image will be created
2. The raw disk image path will be passed to fuse-badsector-simulator along with
   a list of bad sectors and a number of reserve sectors for reallocation.
3. The mirror disk image under the FUSE mount point may be utilized directly
   using dd and other tools, or it may be attached to a virtual machine and
   utilized by the associated virtual disk framework.
4. I/O operations performed on the mirror disk image file for sectors that have
   not been marked bad will be passed through to the raw disk image file.
5. Read operations performed on sectors that have been marked bad will fail to
   read data and will return an I/O error.
6. Write operations performed on sectors that have been marked bad will check
   the sector reserve to see if reserve sectors are available for reallocation.
   If reserve sectors are available, the bad sectors will be virtually
   reallocated, the data will be written to the raw disk image file, and the
   reserve sector count will be decremented. If reserve sectors are not
   available, the write operation will fail and return an I/O error.

### Mission

The goal for fuse-badsector-simulator is to provide a tool that can simulate
physical disk sectors going bad in a predictable way. It is meant to facilitate
development and testing of other tools that handle such disk issues for physical
disks. Since physical disk errors are difficult to produce in a predictable and
consistent manner, this tool provides a means to do so in a virtual environment.

### Getting Started

This project is built with cmake and uses FUSE version 26. The appropriate FUSE
headers and libraries need to be available and cmake must be installed.

To build from the project directory:

    cmake .
    make

A fuse-badsector-simulator binary will be output to the bin subdirectory.

    Usage: fuse-badsector-simulator mountpoint [options]

    General options:
        -h   --help            print help
        -V   --version         print version
        -i   --diskimage       path to disk image to filter
        -s   --badsectors      list of bad sectors, use , to delimit and - for ranges []
        -r   --reservesectors  number of reserve sectors for reallocation [0]

All standard FUSE command-line options are supported as well.

### Bugs

If you find a bug, please feel free to create a
[GitHub issue](https://github.com/att-comdev/fuse-badsector-simulator/issues)
