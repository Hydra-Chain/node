sudo su User
cd
cd workspace/LockTrip/
echo "git pull https://locktrip.gcloud:gallin01@gitlab.com/LockTrip-Dev-Team/LockTrip.git"
echo "git submodule update --recursive"
echo "./autogen.sh"
echo "./configure"
echo "make -j2"
echo "pkill -9 locktripd"
echo "./src/locktripd -testnet -daemon"
exit
exit


