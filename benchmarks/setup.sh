#!/bin/sh

echo "Installing Packages for Lighttpd benchmark [lighttpd, wrk]"
pkg install -y lighttpd
pkg install -y wrk
pkg install -y pidof

echo "Installing python [Python3.7]"
pkg install -y python37-3.7.8
python3.7 -m venv ~/venv
. ~/venv/bin/activate

echo "Installing Packages for pip in virtualenv"
python -m pip install --upgrade pip
python -m pip install -r requirements


