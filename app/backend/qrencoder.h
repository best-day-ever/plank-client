#pragma once

// Small QR Code encoder (ISO/IEC 18004, model 2): byte mode, versions 1-40,
// the four error correction levels, automatic or fixed mask. It exists so the
// Client can show an authenticator enrolment URI (otpauth://) as a QR code
// without a network service or an extra library. Pure and dependency-light;
// tested against an independent encoder in tests/plankbroker.

#include <QByteArray>
#include <QVector>

namespace QrEncoder
{

enum class Ecc { Low, Medium, Quartile, High };

struct Matrix {
    int version = 0;         // 1..40; 0 when encoding failed
    int size = 0;            // modules per side (17 + 4 * version)
    int mask = -1;           // 0..7
    Ecc ecc = Ecc::Medium;
    QVector<bool> modules;   // row-major, true = dark

    bool isValid() const { return version > 0 && modules.size() == size * size; }
    bool dark(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < size && y < size && modules.at(y * size + x);
    }
};

// Encodes `data` in byte mode with the smallest version that fits between
// minimumVersion and maximumVersion. mask -1 picks the mask with the lowest
// penalty score (as the standard requires); 0..7 forces one. Returns an
// invalid Matrix when the data does not fit or an argument is out of range.
Matrix encode(const QByteArray& data, Ecc ecc = Ecc::Medium, int mask = -1,
              int minimumVersion = 1, int maximumVersion = 40);

// Number of data bytes a byte-mode symbol of this version and level holds.
int byteCapacity(int version, Ecc ecc);

}
