# srsRAN

## To edit LTE bands in UE capability

To edit the bands requested as part of UE capability, edit the file found at `srsenb/src/stack/rrc/rrc_ue.cc`.

You can set the bands in the `send_ue_cap_enquiry` function body.

## To edit NR bands in UE capability

TBD

## Running locally

### Open5GS

Open5GS handles the 5G/4G core which srsRAN will connect to.

Follow these instructions: https://open5gs.org/open5gs/docs/guide/01-quickstart/

### Install dependencies

```
sudo apt-get install build-essential cmake libfftw3-dev libmbedtls-dev libboost-program-options-dev libconfig++-dev libsctp-dev
```

### Build srsRAN

```
mkdir build
cd build
cmake ../
sudo make -j$((`nproc`+1))
sudo make install
sudo srsenb
```

This builds and launches srsRAN's eNB. Config files can be edited normally (usually in `/root/.config/srsran/...`).
