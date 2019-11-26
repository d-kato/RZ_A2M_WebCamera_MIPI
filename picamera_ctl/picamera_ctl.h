
#ifndef PICAMERA_CTL_H
#define PICAMERA_CTL_H

#include "mbed.h"

class picamera_ctl {

public:

    /** picamera_ctl
     *
     *  @param sda I2C data line pin
     *  @param scl I2C clock line pin
     */
    picamera_ctl(PinName sda, PinName scl);

    /** Set exposure speed
     *
     *  @param data exposure speed
     */
    void SetExposureSpeed(uint16_t data);

    /** Get exposure speed
     *
     *  @param data exposure speed
     */
    uint16_t GetExposureSpeed();

private:
    I2C mI2c_;
};

#endif
