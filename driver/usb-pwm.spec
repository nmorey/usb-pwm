# spec file for package usb-pwm-hwmon
#
# Copyright (c) 2026 Nathan Morey
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself.

# norootforbuild
%define         git_ver ${nil}

Name:           usb-pwm
Version:        1.0
Release:        0
Summary:        USB HWMON Fan PWM Controller Kernel Module
License:        GPL-2.0-only
Group:          System/Kernel
URL:            https://github.com/nmorey/usb-pwm
Source0:        %{name}-%{version}%{?git_ver}.tar.gz
BuildRequires:  %kernel_module_package_buildreqs
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

# Generates the KMP subpackages (e.g. usb-pwm-hwmon-kmp-default)
%kernel_module_package

%description
This package contains the out-of-tree Linux kernel driver for the USB PWM
Fan Controller (Arduino Nano R4). It registers with the hwmon subsystem
to expose standard sysfs interfaces (fan1_input, fan2_input, pwm1, pwm2)
for PC fan control and monitoring.

%prep
%setup -q -n  %{name}-%{version}%{?git_ver}

%build
# Build the module for each active kernel flavor
for flavor in %flavors_to_build; do
    rm -rf obj/$flavor
    cp -r source obj/$flavor
    make -C %{kernel_source $flavor} modules M=$PWD/obj/$flavor
done

%install
# Install the module in the updates/ subdirectory so it takes precedence
export INSTALL_MOD_PATH=%{buildroot}
export INSTALL_MOD_DIR=updates
for flavor in %flavors_to_build; do
    make -C %{kernel_source $flavor} modules_install M=$PWD/obj/$flavor
done

%clean
rm -rf %{buildroot}

%changelog
