#include "zcm/transport.h"
#include "zcm/transport_registrar.h"
#include "zcm/transport_register.hpp"
#include "zcm/util/lockfile.h"
#include "zcm/util/debug.h"

#include "generic_serial_transport.h"

#include "util/TimeUtil.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include <cassert>
#include <cstring>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
using namespace std;

// TODO: This transport layer needs to be "hardened" to handle
// all of the possible errors and corner cases. Currently, it
// should work fine in most cases, but it might fail on some
// rare cases...

// Define this the class name you want
#define ZCM_TRANS_CLASSNAME TransportSerial
#define MTU (1<<14)
#define ESCAPE_CHAR (0xcc)

#define SERIAL_TIMEOUT_US 1e5 // u-seconds

#define US_TO_MS(a) (a)/1e3

struct Serial
{
    Serial(){}
    ~Serial() { close(); }

    zbool_t open(const zstring_t& port, zint32_t baud, zbool_t hwFlowControl);
    zbool_t isOpen() { return fd > 0; };
    void close();

    zint32_t write(const zuint8_t* buf, zuint32_t sz);
    zint32_t read(zuint8_t* buf, zuint32_t sz, zuint64_t timeoutMs);
    // Returns 0 on invalid input baud otherwise returns termios constant baud value
    static zint32_t convertBaud(zint32_t baud);

    Serial(const Serial&) = delete;
    Serial(Serial&&) = delete;
    Serial& operator=(const Serial&) = delete;
    Serial& operator=(Serial&&) = delete;

  private:
    zstring_t port;
    zint32_t fd = -1;
};

zbool_t Serial::open(const zstring_t& port_, zint32_t baud, zbool_t hwFlowControl)
{
    if (baud == 0) {
        fprintf(stderr, "Serial baud rate not specified in url. "
                        "Proceeding without setting baud\n");
    } else if (!(baud = convertBaud(baud))) {
        fprintf(stderr, "Unrecognized baudrate. Failed to open serial device.\n ");
        return zfalse;
    }

    if (!lockfile_trylock(port_.c_str())) {
        ZCM_DEBUG("failed to create lock file, refusing to open serial device (%s)",
                  port_.c_str());
        return zfalse;
    }
    this->port = port_;

    zint32_t flags = O_RDWR | O_NOCTTY | O_SYNC;
    fd = ::open(port.c_str(), flags, 0);
    if (fd < 0) {
        ZCM_DEBUG("failed to open serial device (%s): %s", port.c_str(), strerror(errno));
        goto fail;
    }

    // attempt to reset the USB device
    // this call may fail: but we don't care
    // occasionally the USB bus can get in a weird state
    // this call will reset the USB in that situation
    ioctl(fd, USBDEVFS_RESET, 0);

    struct termios opts;

    // get the termios config
    if (tcgetattr(fd, &opts)) {
        ZCM_DEBUG("failed to get termios options on fd: %s", strerror(errno));
        goto fail;
    }

    if (baud != 0) {
        cfsetispeed(&opts, baud);
        cfsetospeed(&opts, baud);
    }
    cfmakeraw(&opts);

    opts.c_cflag &= ~CSTOPB;
    opts.c_cflag |= CS8;
    opts.c_cflag &= ~PARENB;
    if (hwFlowControl) opts.c_cflag |= CRTSCTS;
    opts.c_cc[VTIME]    = 1;
    opts.c_cc[VMIN]     = 30;

    // set the new termios config
    if (tcsetattr(fd, TCSANOW, &opts)) {
        ZCM_DEBUG("failed to set termios options on fd: %s", strerror(errno));
        goto fail;
    }

    tcflush(fd, TCIOFLUSH);

    return ztrue;

 fail:
    // Close the port if it was opened
    if (fd > 0) {
        const zint32_t saved_errno = errno;
        zint32_t result;
        do {
            result = ::close(fd);
        } while (result == -1 && errno == EINTR);
        errno = saved_errno;
    }
    this->fd = -1;

    // Unlock the lock file
    if (port != "") lockfile_unlock(port.c_str());
    this->port = "";

    return zfalse;
}

void Serial::close()
{
    if (isOpen()) {
        ZCM_DEBUG("Closing!\n");
        ::close(fd);
        fd = 0;
    }
    if (port != "") {
        lockfile_unlock(port.c_str());
        port = "";
    }
}

zint32_t Serial::write(const zuint8_t* buf, zuint32_t sz)
{
    assert(this->isOpen());
    zint32_t ret = ::write(fd, buf, sz);
    if (ret == -1) {
        ZCM_DEBUG("ERR: write failed: %s", strerror(errno));
        return -1;
    }
    return ret;
}

zint32_t Serial::read(zuint8_t* buf, zuint32_t sz, zuint64_t timeoutUs)
{
    assert(this->isOpen());
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    zuint64_t tOut = max((zuint64_t)SERIAL_TIMEOUT_US, timeoutUs);

    struct timeval timeout;
    timeout.tv_sec = tOut / 1000000;
    timeout.tv_usec = tOut % 1000000;
    zint32_t status = ::select(fd + 1, &fds, NULL, NULL, &timeout);

    if (status > 0) {
        if (FD_ISSET(fd, &fds)) {
            zint32_t ret = ::read(fd, buf, sz);
            if (ret == -1) {
                ZCM_DEBUG("ERR: serial read failed: %s", strerror(errno));
            } else if (ret == 0) {
                ZCM_DEBUG("ERR: serial device unplugged");
                close();
                assert(zfalse && "ERR: serial device unplugged\n" &&
                       "ZCM does not support reconnecting to serial devices");
                return -3;
            }
            return ret;
        } else {
            ZCM_DEBUG("ERR: serial bytes not ready");
            return -1;
        }
    } else {
        ZCM_DEBUG("ERR: serial read timed out");
        return -2;
    }
}

zint32_t Serial::convertBaud(zint32_t baud)
{
    switch (baud) {
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        default:
            return 0;
    }
}

struct ZCM_TRANS_CLASSNAME : public zcm_trans_t
{
    Serial ser;

    zint32_t baud;
    zbool_t hwFlowControl;

    zbool_t raw;
    zstring_t rawChan;
    zuint32_t rawSize;
    std::unique_ptr<zuint8_t[]> rawBuf;

    zstring_t address;

    unordered_map<zstring_t, zstring_t> options;

    zcm_trans_t* gst;

    zuint64_t timeoutLeft;

    zstring_t* findOption(const zstring_t& s)
    {
        auto it = options.find(s);
        if (it == options.end()) return nullptr;
        return &it->second;
    }

    ZCM_TRANS_CLASSNAME(zcm_url_t* url)
    {
        trans_type = ZCM_BLOCKING;
        vtbl = &methods;

        // build 'options'
        auto* opts = zcm_url_opts(url);
        for (zuint32_t i = 0; i < opts->numopts; ++i)
            options[opts->name[i]] = opts->value[i];

        baud = 0;
        auto* baudStr = findOption("baud");
        if (!baudStr) {
            fprintf(stderr, "Baud unspecified. Bypassing serial baud setup.\n");
        } else {
            baud = atoi(baudStr->c_str());
            if (baud == 0) {
                ZCM_DEBUG("expected integer argument for 'baud'");
                return;
            }
        }

        hwFlowControl = zfalse;
        auto* hwFlowControlStr = findOption("hw_flow_control");
        if (hwFlowControlStr) {
            if (*hwFlowControlStr == "true") {
                hwFlowControl = ztrue;
            } else if (*hwFlowControlStr == "false") {
                hwFlowControl = zfalse;
            } else {
                ZCM_DEBUG("expected boolean argument for 'hw_flow_control'");
                return;
            }
        }

        raw = zfalse;
        auto* rawStr = findOption("raw");
        if (rawStr) {
            if (*rawStr == "true") {
                raw = ztrue;
            } else if (*rawStr == "false") {
                raw = zfalse;
            } else {
                ZCM_DEBUG("expected boolean argument for 'raw'");
                return;
            }
        }

        rawChan = "";
        auto* rawChanStr = findOption("raw_channel");
        if (rawChanStr) {
            rawChan = *rawChanStr;
        }

        rawSize = 1024;
        auto* rawSizeStr = findOption("raw_size");
        if (rawSizeStr) {
            rawSize = atoi(rawSizeStr->c_str());
            if (rawSize <= 0) {
                ZCM_DEBUG("expected positive integer argument for 'raw_size'");
                return;
            }
        }

        address = zcm_url_address(url);
        ser.open(address, baud, hwFlowControl);

        if (raw) {
            rawBuf.reset(new zuint8_t[rawSize]);
            gst = nullptr;
        } else {
            gst = zcm_trans_generic_serial_create(&ZCM_TRANS_CLASSNAME::get,
                                                  &ZCM_TRANS_CLASSNAME::put,
                                                  this,
                                                  &ZCM_TRANS_CLASSNAME::timestamp_now,
                                                  nullptr,
                                                  MTU, MTU * 10);
        }
    }

    ~ZCM_TRANS_CLASSNAME()
    {
        ser.close();
        if (gst) zcm_trans_generic_serial_destroy(gst);
    }

    zbool_t good()
    {
        return ser.isOpen();
    }

    static zuint32_t get(zuint8_t* data, zuint32_t nData, void* usr)
    {
        ZCM_TRANS_CLASSNAME* me = cast((zcm_trans_t*) usr);
        zuint64_t startUtime = TimeUtil::utime();
        zint32_t ret = me->ser.read(data, nData, me->timeoutLeft);
        zuint64_t diff = TimeUtil::utime() - startUtime;
        me->timeoutLeft = me->timeoutLeft > diff ? me->timeoutLeft - diff : 0;
        return ret < 0 ? 0 : ret;
    }

    static zuint32_t put(const zuint8_t* data, zuint32_t nData, void* usr)
    {
        ZCM_TRANS_CLASSNAME* me = cast((zcm_trans_t*) usr);
        zint32_t ret = me->ser.write(data, nData);
        return ret < 0 ? 0 : ret;
    }

    static zuint64_t timestamp_now(void* usr)
    { return TimeUtil::utime(); }

    /********************** METHODS **********************/
    zuint32_t getMtu()
    { return raw ? MTU : zcm_trans_get_mtu(this->gst); }

    zcm_retcode_t sendmsg(zcm_msg_t msg)
    {
        if (raw) {
            if (put(msg.buf, msg.len, this) != 0) return ZCM_EOK;
            return ZCM_EAGAIN;
        } else {
            // Note: No need to lock here ONLY because the internals of
            //       generic serial transport sendmsg only use the sendBuffer
            //       and touch no variables related to receiving
            zcm_retcode_t ret = zcm_trans_sendmsg(this->gst, msg);
            if (ret != ZCM_EOK) return ret;
            return serial_update_tx(this->gst);
        }
    }

    zcm_retcode_t recvmsgEnable(const zchar_t* channel, zbool_t enable)
    { return raw ? ZCM_EOK : zcm_trans_recvmsg_enable(this->gst, channel, enable); }

    zcm_retcode_t recvmsg(zcm_msg_t* msg, zint32_t timeoutMs)
    {
        timeoutLeft = timeoutMs > 0 ? timeoutMs * 1e3 : numeric_limits<zuint64_t>::max();

        if (raw) {
            zuint32_t sz = get(rawBuf.get(), rawSize, this);
            if (sz == 0 || rawChan.empty()) return ZCM_EAGAIN;

            msg->utime   = timestamp_now(this);
            msg->channel = rawChan.c_str();
            msg->len     = sz;
            msg->buf     = rawBuf.get();

            return ZCM_EOK;
        } else {
            do {
                zuint64_t startUtime = TimeUtil::utime();

                // Note: No need to lock here ONLY because the internals of
                //       generic serial transport recvmsg only use the recv related
                //       data members and touch no variables related to sending
                zcm_retcode_t ret = zcm_trans_recvmsg(this->gst, msg, timeoutLeft);
                if (ret == ZCM_EOK) return ret;

                zuint64_t diff = TimeUtil::utime() - startUtime;
                startUtime = TimeUtil::utime();
                // Note: timeoutLeft is calculated here because serial_update_rx
                //       needs it to be set properly so that the blocking read in
                //       `get` knows how long it has to exit
                timeoutLeft = timeoutLeft > diff ? timeoutLeft - diff : 0;

                serial_update_rx(this->gst);

                diff = TimeUtil::utime() - startUtime;
                timeoutLeft = timeoutLeft > diff ? timeoutLeft - diff : 0;

            } while (timeoutLeft > 0);

            return ZCM_EAGAIN;
        }
    }

    /********************** STATICS **********************/
    static zcm_trans_methods_t methods;
    static ZCM_TRANS_CLASSNAME* cast(zcm_trans_t* zt)
    {
        assert(zt->vtbl == &methods);
        return (ZCM_TRANS_CLASSNAME*)zt;
    }

    static zuint32_t _getMtu(zcm_trans_t* zt)
    { return cast(zt)->getMtu(); }

    static zcm_retcode_t _sendmsg(zcm_trans_t* zt, zcm_msg_t msg)
    { return cast(zt)->sendmsg(msg); }

    static zcm_retcode_t _recvmsgEnable(zcm_trans_t* zt, const zchar_t* channel, zbool_t enable)
    { return cast(zt)->recvmsgEnable(channel, enable); }

    static zcm_retcode_t _recvmsg(zcm_trans_t* zt, zcm_msg_t* msg, zint32_t timeout)
    { return cast(zt)->recvmsg(msg, timeout); }

    static void _destroy(zcm_trans_t* zt)
    { delete cast(zt); }

    static const TransportRegister reg;
};

zcm_trans_methods_t ZCM_TRANS_CLASSNAME::methods = {
    &ZCM_TRANS_CLASSNAME::_getMtu,
    &ZCM_TRANS_CLASSNAME::_sendmsg,
    &ZCM_TRANS_CLASSNAME::_recvmsgEnable,
    &ZCM_TRANS_CLASSNAME::_recvmsg,
    NULL, // update
    &ZCM_TRANS_CLASSNAME::_destroy,
};

static zcm_trans_t* create(zcm_url_t* url)
{
    auto* trans = new ZCM_TRANS_CLASSNAME(url);
    if (trans->good())
        return trans;

    delete trans;
    return nullptr;
}

#ifdef USING_TRANS_SERIAL
// Register this transport with ZCM
const TransportRegister ZCM_TRANS_CLASSNAME::reg(
    "serial", "Transfer data via a serial connection "
              "(e.g. 'serial:///dev/ttyUSB0?baud=115200&hw_flow_control=true' or "
              "'serial:///dev/pts/10?raw=true&raw_channel=RAW_SERIAL')",
    create);
#endif
