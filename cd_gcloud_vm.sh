sudo su User
cd
cd workspace/LockTrip/
git pull https://locktrip.gcloud:gallin01@gitlab.com/LockTrip-Dev-Team/LockTrip.git
echo "git submodule update --recursive"
echo "./autogen.sh"
echo "./configure"
echo "make -j2"
echo "./src/locktripd -testnet -daemon"
exit
exit


