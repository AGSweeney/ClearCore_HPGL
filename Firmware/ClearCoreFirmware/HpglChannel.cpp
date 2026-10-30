/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "HpglChannel.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kEsc = 0x1b;

/* Device-control commands that take ';'-separated parameters ending in ':'. */
static bool TakesParams(char cmd) {
    return strchr("@HIMNSTUP", cmd) != nullptr;
}

HpglChannel::HpglChannel()
    : m_io(nullptr),
      m_mem(nullptr),
      m_size(0),
      m_head(0),
      m_count(0),
      m_dc(DC_DATA),
      m_dcCmd(0),
      m_dcParamCount(0),
      m_dcHaveDigit(false),
      m_on(true),
      m_configHandshake(HPGL_HS_NONE),
      m_xonxoff(false),
      m_xoffSent(false),
      m_xoffChar(kXoff),
      m_xonLen(1),
      m_enq(0),
      m_enqThreshold(80),
      m_ackLen(0),
      m_ackPending(false),
      m_termLen(1),
      m_extError(HPGL_EXT_NONE),
      m_abortRequested(false),
      m_overflows(0),
      m_bytesIn(0) {
    m_xonChars[0] = kXon;
    m_term[0] = '\r';
    m_term[1] = 0;
}

void HpglChannel::Init(HpglChannelIo *io, uint8_t *mem, uint16_t size, uint8_t handshake) {
    m_io = io;
    m_mem = mem;
    m_size = size;
    m_head = 0;
    m_count = 0;
    m_configHandshake = handshake;
    ResetDeviceControl();
}

void HpglChannel::SetHandshake(uint8_t handshake) {
    m_configHandshake = handshake;
    m_xonxoff = (handshake == HPGL_HS_XONXOFF);
    if (!m_xonxoff && m_xoffSent) {
        m_xoffSent = false;
    }
}

void HpglChannel::ResetDeviceControl() {
    m_dc = DC_DATA;
    m_on = true;
    m_xonxoff = (m_configHandshake == HPGL_HS_XONXOFF);
    m_xoffSent = false;
    m_xoffChar = kXoff;
    m_xonChars[0] = kXon;
    m_xonLen = 1;
    m_enq = 0;
    m_enqThreshold = 80;
    m_ackLen = 0;
    m_ackPending = false;
    m_term[0] = '\r';
    m_termLen = 1;
    m_extError = HPGL_EXT_NONE;
}

void HpglChannel::Push(uint8_t c) {
    const uint16_t tail = (uint16_t)((m_head + m_count) % m_size);
    m_mem[tail] = c;
    m_count++;
}

bool HpglChannel::Pop(uint8_t *c) {
    if (m_count == 0) {
        return false;
    }
    *c = m_mem[m_head];
    m_head = (uint16_t)((m_head + 1) % m_size);
    m_count--;
    return true;
}

void HpglChannel::ClearData() {
    m_head = 0;
    m_count = 0;
}

bool HpglChannel::TakeAbortRequest() {
    const bool r = m_abortRequested;
    m_abortRequested = false;
    return r;
}

void HpglChannel::SendByte(uint8_t c) {
    if (m_io) {
        m_io->ChannelSend(&c, 1);
    }
}

void HpglChannel::SendRaw(const char *text) {
    if (m_io && text) {
        m_io->ChannelSend((const uint8_t *)text, (uint16_t)strlen(text));
    }
}

void HpglChannel::SendReply(const char *body) {
    SendRaw(body);
    if (m_io && m_termLen > 0) {
        m_io->ChannelSend(m_term, m_termLen);
    }
}

void HpglChannel::SendNumber(int32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)v);
    SendReply(buf);
}

int32_t HpglChannel::Param(uint8_t i, int32_t dflt) const {
    if (i >= m_dcParamCount || m_dcParams[i] < 0) {
        return dflt;
    }
    return m_dcParams[i];
}

void HpglChannel::Poll() {
    if (m_xonxoff) {
        const uint16_t high = (uint16_t)((m_size / 8 > 64) ? m_size / 8 : 64);
        if (!m_xoffSent && Free() < high) {
            SendByte(m_xoffChar);
            m_xoffSent = true;
        } else if (m_xoffSent && Free() > m_size / 2) {
            if (m_io) {
                m_io->ChannelSend(m_xonChars, m_xonLen);
            }
            m_xoffSent = false;
        }
    }
    if (m_ackPending && Free() >= m_enqThreshold) {
        m_ackPending = false;
        if (m_io && m_ackLen > 0) {
            m_io->ChannelSend(m_ack, m_ackLen);
        }
    }
}

bool HpglChannel::Ingest(uint8_t c) {
    m_bytesIn++;
    switch (m_dc) {
        case DC_ESC:
            if (c == '.') {
                m_dc = DC_DOT;
            } else if (c == '%' || c == '&' || c == '*' || c == '(' || c == ')') {
                m_dc = DC_PCL;
            } else if (c == 'E') {
                m_dc = DC_DATA;  /* PCL reset */
            } else {
                m_dc = DC_DATA;
                return Ingest(c);
            }
            return true;

        case DC_PCL:
            /* PCL/PJL escape ends at an upper-case letter (ESC%-12345X, ESC%1B, ESC&l0O). */
            if (c >= 'A' && c <= 'Z') {
                m_dc = DC_DATA;
            } else if (c == kEsc) {
                m_dc = DC_ESC;
            }
            return true;

        case DC_DOT:
            m_dcCmd = (char)c;
            m_dcParamCount = 0;
            m_dcHaveDigit = false;
            if (TakesParams(m_dcCmd)) {
                m_dc = DC_PARAMS;
            } else {
                m_dc = DC_AFTER;
                ExecuteDeviceControl(m_dcCmd);
            }
            return true;

        case DC_PARAMS:
            if (c >= '0' && c <= '9') {
                if (!m_dcHaveDigit) {
                    if (m_dcParamCount >= 8) {
                        m_extError = HPGL_EXT_TOO_MANY_PARAMS;
                        return true;
                    }
                    m_dcParams[m_dcParamCount++] = 0;
                    m_dcHaveDigit = true;
                }
                int32_t &v = m_dcParams[m_dcParamCount - 1];
                if (v < 100000) {
                    v = v * 10 + (c - '0');
                }
                return true;
            }
            if (c == ';') {
                if (!m_dcHaveDigit && m_dcParamCount < 8) {
                    m_dcParams[m_dcParamCount++] = -1;  /* omitted */
                }
                m_dcHaveDigit = false;
                return true;
            }
            if (c == ':') {
                m_dc = DC_DATA;
                ExecuteDeviceControl(m_dcCmd);
                return true;
            }
            if (c == ' ') {
                return true;
            }
            m_extError = HPGL_EXT_BAD_DC_PARAM;
            m_dc = DC_DATA;
            return Ingest(c);

        case DC_AFTER:
            m_dc = DC_DATA;
            if (c == ':') {
                return true;  /* optional terminator after a no-parameter command */
            }
            return Ingest(c);

        case DC_DATA:
            break;
    }

    if (c == kEsc) {
        m_dc = DC_ESC;
        return true;
    }
    if (m_xonxoff && (c == kXon || c == kXoff)) {
        return true;  /* host pacing our output; not data */
    }
    if (m_enq != 0 && c == m_enq) {
        if (Free() >= m_enqThreshold) {
            if (m_io && m_ackLen > 0) {
                m_io->ChannelSend(m_ack, m_ackLen);
            }
        } else {
            m_ackPending = true;
        }
        return true;
    }
    if (!m_on) {
        return true;  /* programmed off: data is discarded */
    }
    if (m_count >= m_size) {
        m_overflows++;
        m_extError = HPGL_EXT_BUFFER_OVERFLOW;
        return false;
    }
    Push(c);
    return true;
}

void HpglChannel::ExecuteDeviceControl(char cmd) {
    switch (cmd) {
        case '(':
        case 'Y':
            m_on = true;
            break;
        case ')':
        case 'Z':
            m_on = false;
            break;
        case 'B':
            SendNumber(Free());
            break;
        case 'L':
            SendNumber(m_size);
            break;
        case 'E':
            SendNumber(m_extError);
            m_extError = HPGL_EXT_NONE;
            break;
        case 'O':
            SendNumber(m_count == 0 ? 8 : 0);
            break;
        case 'J':
            m_ackPending = false;
            break;
        case 'K':
            ClearData();
            m_abortRequested = true;
            break;
        case 'R':
            ResetDeviceControl();
            break;
        case 'H':
        case 'I': {
            /* block size (threshold); ENQ char; up to three ACK / XON chars */
            m_enqThreshold = (uint16_t)Param(0, 80);
            if (m_enqThreshold > m_size) {
                m_enqThreshold = m_size;
            }
            m_enq = (uint8_t)Param(1, 0);
            m_ackLen = 0;
            for (uint8_t i = 2; i < m_dcParamCount && m_ackLen < 3; i++) {
                if (m_dcParams[i] >= 0) {
                    m_ack[m_ackLen++] = (uint8_t)m_dcParams[i];
                }
            }
            if (cmd == 'I' && m_ackLen > 0) {
                for (uint8_t i = 0; i < m_ackLen; i++) {
                    m_xonChars[i] = m_ack[i];
                }
                m_xonLen = m_ackLen;
            }
            break;
        }
        case 'N': {
            /* intercharacter delay; XOFF trigger char -> XON/XOFF pacing */
            const int32_t xoff = Param(1, -1);
            if (xoff > 0) {
                m_xoffChar = (uint8_t)xoff;
                m_xonxoff = true;
            }
            break;
        }
        case 'M': {
            /* turnaround delay; output trigger; echo terminate; terminator 1; terminator 2; initiator */
            m_term[0] = (uint8_t)Param(3, '\r');
            m_termLen = 1;
            const int32_t t2 = Param(4, -1);
            if (t2 > 0) {
                m_term[1] = (uint8_t)t2;
                m_termLen = 2;
            }
            break;
        }
        case '@':
        case 'S':
        case 'T':
        case 'U':
        case 'P':
            break;  /* buffer allocation / logical I-O config: accepted */
        default:
            m_extError = HPGL_EXT_BAD_ESC_BYTE;
            break;
    }
}
