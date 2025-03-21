package=boost

$(package)_version=1_72_0
$(package)_download_path=https://github.com/KomodoPlatform/boost/releases/download/boost-1.72.0-kmd
$(package)_sha256_hash=59c9b274bc451cf91a9ba1dd2c7fdcaf5d60b1b3aa83f2c9fa143417cc660722
$(package)_file_name=$(package)_$($(package)_version).tar.bz2
$(package)_patches=commit-74fb0a2.patch commit-f9d0e59.patch ignore_wnonnull_gcc_11.patch range_enums_clang_16.patch

define $(package)_set_vars
$(package)_config_opts_release=variant=release
$(package)_config_opts_debug=variant=debug
$(package)_config_opts=--layout=system --user-config=user-config.jam
$(package)_config_opts+=threading=multi link=static -sNO_BZIP2=1 -sNO_ZLIB=1
$(package)_config_opts_linux=threadapi=pthread runtime-link=shared
$(package)_config_opts_darwin=--toolset=gcc runtime-link=shared threadapi=pthread
$(package)_config_opts_mingw32=binary-format=pe target-os=windows threadapi=win32 runtime-link=static
$(package)_config_opts_x86_64_mingw32=address-model=64
$(package)_config_opts_i686_mingw32=address-model=32
$(package)_config_opts_i686_linux=address-model=32 architecture=x86
$(package)_toolset_$(host_os)=gcc
$(package)_archiver_$(host_os)=$($(package)_ar)
$(package)_toolset_darwin=gcc
$(package)_archiver_darwin=$($(package)_ar)
$(package)_config_libraries=chrono,filesystem,program_options,system,thread,test
$(package)_cxxflags+=-std=c++11 -fvisibility=hidden
$(package)_cxxflags_linux=-fPIC
endef


define $(package)_preprocess_cmds
  patch -p2 -i $($(package)_patch_dir)/commit-74fb0a2.patch && \
  patch -p2 -i $($(package)_patch_dir)/commit-f9d0e59.patch && \
  patch -p2 -i $($(package)_patch_dir)/ignore_wnonnull_gcc_11.patch && \
  patch -p2 -i $($(package)_patch_dir)/range_enums_clang_16.patch && \
  echo "using $(boost_toolset_$(host_os)) : : $($(package)_cxx) : <cxxflags>\"$($(package)_cxxflags) $($(package)_cppflags)\" <linkflags>\"$($(package)_ldflags)\" <archiver>\"$(boost_archiver_$(host_os))\" <striper>\"$(host_STRIP)\"  <ranlib>\"$(host_RANLIB)\" <rc>\"$(host_WINDRES)\" : ;" > user-config.jam
endef

define $(package)_config_cmds
  ./bootstrap.sh --without-icu --with-libraries=$(boost_config_libraries)
endef

ifeq ($(host_os),linux)
define $(package)_build_cmds
  ./b2 -d2 -j2 -d1 --prefix=$($(package)_staging_prefix_dir) $($(package)_config_opts) cxxflags=-std=c++11 stage
endef
define $(package)_stage_cmds
  ./b2 -d0 -j4 --prefix=$($(package)_staging_prefix_dir) $($(package)_config_opts) cxxflags=-std=c++11 install
endef
else
define $(package)_build_cmds
  ./b2 -d2 -j2 -d1 --prefix=$($(package)_staging_prefix_dir) $($(package)_config_opts) stage
endef
define $(package)_stage_cmds
  ./b2 -d0 -j4 --prefix=$($(package)_staging_prefix_dir) $($(package)_config_opts) install
endef
endif
