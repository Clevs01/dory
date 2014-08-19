## Setting Up the Google Test Framework

For simplicity, the following instructions assume that you are running on an
x86-64 architecture Linux installation.  For 32-bit installations, the
libraries would get copied to /usr/lib rather than /usr/lib64.  To set up the
Google Test Framework, execute the following commands:

```
wget http://googletest.googlecode.com/files/gtest-1.7.0.zip
unzip gtest-1.7.0.zip
cd gtest-1.7.0
./configure
make
cd lib/.libs
```

If you are running on CentOS or RHEL then:

```
mv *.a *.lai *.so* ../*.la /usr/lib64
```

If you are running on Ubuntu then:

```
mv *.a *.lai *.so* ../*.la /usr/lib
```

Now evecute the following commands (for either distro):

```
ldconfig
cd ../..
cp -a include/gtest /usr/include
```

Now [continue setting up your build environment](https://github.com/tagged/bruce/blob/master/README.md#setting-up-a-build-environment).