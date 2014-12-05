#pragma once

#include <string>
#include <vector>
#include <array>
#include <ctime>

#include "CArcDevice/CArcPCIe.h"
#include "CArcDevice/CArcDevice.h"
#include "CArcDeinterlace/CArcDeinterlace.h"

#include "arcticICC/basics.h"

namespace arctic {

    static int const CCDWidth = 4096;   // width of CCD (unbinned pixels)
    static int const CCDHeight = 4096;  // width of CCD (unbinned pixels)
    static int const XOverscan = 50;    // x overscan per amplifier (unbinned pixels)
    static int const XNumAmps = 2;      // number of amplifiers along the x axis
    static int const DeinterlaceAlgorithm = arc::deinterlace::CArcDeinterlace::DEINTERLACE_CCD_QUAD;

    /**
    ARCTIC imager CCD

    This is a thin wrapper around the Leach API; it adds the "Resource Acquisition Is Initialization"
    pattern and simplifies some call signatures.

    @todo
    - fix time measurement while exposing; presently I use time() which is only good to one second;
        use chrono instead (but then how to get the date of the exposure?)
    - windowing is forbidden unless only reading from one amplifier, so:
        - when setWindow is called with a non-full window, automatically switch to reading only one amplifier;
            this affects the image size (only one overscan) and the location of the bias regions, so update that data
            based on the current readout mode. How clever do we have to be? Need we pick a particular amplifier
            based on where the subregion is (does it increase speed)?
            - Should we move this logic into the python code and have setting readout mode separate from windowing (likely yes,
            but it leads to a need for more error checking).
        - when full windowing is requested switch back to quad amplifiers
    - the readout rate is forced at startup and is cached; if there was a way to read it from the controller
      that might be nicer, or a way of knowing what value it has at startup
    */
    class Camera {
    public:
        /**
        Contruct a camera: open the camera and allocate all resources

        Note that the Leach controller does not support y overscan.
        */
        explicit Camera();

        /**
        Destructor: release all allocated resources
        */
        ~Camera();

        /**
        Return true if not idle (i.e. if exposing, paused exposure, or reading out)
        */
        bool isBusy() { return getExposureState().isBusy(); }

        /**
        Start an exposure

        @param[in] expTime  exposure time in seconds
        @param[in] expType  exposure type
        @param[in] expName  name of exposure FITS file
        @throw std::runtime_error if an exposure is in progress
        @throw std::runtime_error if expType is ExposureType::Bias and expTime > 0
        */
        void startExposure(float expTime, ExposureType expType, std::string const &expName);

        /**
        Pause an exposure, closing the shutter if it was open

        @throw std::runtime_error if no exposure is paused
        */
        void pauseExposure();

        /**
        Resume a paused exposure, opening the shutter if that's how the exposure was started

        @throw std::runtime_error if no exposure is paused
        */
        void resumeExposure();

        /**
        Abort an exposure or readout, discarding the data

        @throw std::runtime_error if there is no exopsure to abort
        */
        void abortExposure();

        /**
        Stop an exposure, saving the data

        @throw std::runtime_error if there is no exopsure to stop
        */
        void stopExposure();

        /**
        Get current exposure state
        */
        ExposureState getExposureState();

        /**
        Get bin factor

        @return bin factor in x (cols), y (rows)
        */
        std::array<int, 2> getBinFactor() const { return std::array<int, 2>{_colBinFac, _rowBinFac}; };

        /**
        Set bin factor

        @param[in] colBinFac: number of columns per bin
        @param[in] rowBinFac: number of rows per bin

        The resulting number of binned columns = window width (unbinned) / colBinFac
        using truncated integer division. Similarly for rows.

        @throw std::runtime_error if:
        - colBinFac or rowBinFac < 1 or > unbinned image size
        - an exposure is in progress
        */
        void setBinFactor(int colBinFac, int rowBinFac);

        /**
        Get the image window

        @return window colStart, rowStart, width, height, all in unbinned pixels
        */
        std::array<int, 4> getWindow() const {
            return std::array<int, 4> {_winRowStart, _winColStart, _winWidth, _winHeight};
        };

        /**
        Set image window

        @param[in] colStart: start column (unbinned pixels; 0 is the first column)
        @param[in] rowStart: start row (unbinned pixels; 0 is the first row)
        @param[in] width: number of columns in the data (unbinned pixels)
        @param[in] height: number of rows in the data (unbinned pixels)

        @note you may specify 0,0,0,0 to specify the full frame
        @note the binned image size is width/colBinFac, height/rowBinFac, using truncated integer division

        @throw std::runtime_error if the requested image extends off the imaging area
        */
        void setWindow(int colStart, int rowStart, int width, int height);

        /**
        Get the readout rate
        */
        ReadoutRate getReadoutRate() const;

        /**
        Set the readout rate
        */
        void setReadoutRate(ReadoutRate readoutRate);

        /**
        Save image to a FITS file

        @param[in] expTime  exposure time (sec); if <0 then the internal timer is used.
            If you have feedback from the shutter then you can provide a better value than the internal timer.

        @throw std::runtime_error if no image is available to be saved
        */
        void saveImage(double expTime=-1);

        /**
        Open the shutter

        @throw std::runtime_error if an exposure is in progress
        */
        void openShutter();

        /**
        Close the shutter

        @throw std::runtime_error if an exposure is in progress
        */
        void closeShutter();

        /**
        Return image width, in binned pixels (includes overscan)
        */
        int getImageWidth() const { return (_winWidth + (2 * XOverscan)) / _colBinFac; }

        /**
        Return image height, in binned pixels (will include overscan, if y overscan is ever supported)
        */
        int getImageHeight() const { return _winHeight / _rowBinFac; }

    private:
        void assertIdle();  /// assert that the camera is not busy
        void _setIdle();    /// set values indicating idle state (_cmdExpSec, _fullReadTime and _isPaused)
        double _readTime(int nPix) const; /// estimate readout time (sec) based on number of pixels to read
        /**
        Run one command, as described in the Leach document Controller Command Description
        @param[in] boardID  controller board code: one of TIM_ID, PCI_ID or UTIL_ID
        @param[in] cmd  command code
        @param[in] arg1  argument 1 (optional)
        @param[in] arg2  argument 2 (optional)
        @param[in] arg3  argument 3 (optional)
        */
        void runCommand(std::string const &descr, int boardID, int cmd, int arg1=0, int arg2=0, int arg3=0);
        // it would be safer to read the following parameters directly from the controller,
        // but I don't know how to do that
        ReadoutRate _readoutRate;
        int _colBinFac;     /// column bin factor
        int _rowBinFac;     /// row bin factor
        int _winColStart;   /// starting column for data subwindow (unbinned pixels, starting from 0)
        int _winRowStart;   /// starting row for data subwindow (unbinned pixels, starting from 0)
        int _winWidth;      /// window width (unbinned pixels)
        int _winHeight;     /// window height (unbinned pixels)
        std::string _expName;       /// exposure name (used for the FITS file)
        ExposureType _expType;      /// exposure type
        double _cmdExpSec;          /// commanded exposure time, in seconds; <0 if no exposure
        double _segmentExpSec;      /// exposure time for this segment; updated when an exposure is paused
        time_t _segmentStartTime;   /// start time of this exposure segment; updated when an exposure is paused or resumed;
                                    /// 0 if exposure is paused or not exposing
        double _pauseTime;          /// accumulated exposure pause time (not including current pause, if paused) (sec)
        double _pauseStartTime;     /// start time of current pause

        arc::device::CArcPCIe   _device;  /// the Leach API's representation of a camera controller
    };

} // namespace