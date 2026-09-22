#include "qrencoder.h"

#include <QtGlobal>

#include <cstdlib>
#include <limits>

// Straightforward implementation of ISO/IEC 18004:2015 for byte-mode data:
// segment bits, Reed-Solomon blocks over GF(256) (x^8+x^4+x^3+x^2+1), the
// function patterns, zig-zag codeword placement, the eight data masks and the
// four penalty rules (N1=3, N2=3, N3=40, N4=10).

namespace QrEncoder
{
namespace
{

// Indexed [level][version]; index 0 is unused. Levels in Ecc order (L, M, Q, H).
const int EccCodewordsPerBlock[4][41] = {
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
         28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
         26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
         28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
         30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};

const int ErrorCorrectionBlocks[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
         8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
         17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20,
         23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
         25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};

// Format information level indicators: L=01, M=00, Q=11, H=10.
const int FormatLevelBits[4] = {1, 0, 3, 2};

int level(Ecc ecc) { return static_cast<int>(ecc); }

// Modules left for data and error correction once every function pattern
// (finders, separators, timing, alignment, format and version areas) is placed.
int rawDataModules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int alignments = version / 7 + 2;
        result -= (25 * alignments - 10) * alignments - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int dataCodewords(int version, Ecc ecc)
{
    return rawDataModules(version) / 8 -
            EccCodewordsPerBlock[level(ecc)][version] * ErrorCorrectionBlocks[level(ecc)][version];
}

int characterCountBits(int version) { return version <= 9 ? 8 : 16; }

QVector<int> alignmentPositions(int version, int size)
{
    if (version == 1) return {};
    const int count = version / 7 + 2;
    const int step = (version * 8 + count * 3 + 5) / (count * 4 - 4) * 2;
    QVector<int> positions(count);
    positions[0] = 6;
    for (int i = count - 1, position = size - 7; i >= 1; --i, position -= step) {
        positions[i] = position;
    }
    return positions;
}

quint8 gfMultiply(quint8 a, quint8 b)
{
    int result = 0;
    int x = a;
    for (int y = b; y != 0; y >>= 1) {
        if (y & 1) result ^= x;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    return static_cast<quint8>(result);
}

// Generator polynomial of the given degree, highest coefficient first
// (the leading 1 is implicit and omitted).
QVector<quint8> generator(int degree)
{
    QVector<quint8> polynomial(1, 1);
    quint8 root = 1;
    for (int i = 0; i < degree; ++i) {
        QVector<quint8> next(polynomial.size() + 1, 0);
        for (int k = 0; k < polynomial.size(); ++k) {
            next[k] ^= polynomial[k];
            next[k + 1] ^= gfMultiply(polynomial[k], root);
        }
        polynomial = next;
        root = gfMultiply(root, 2);
    }
    polynomial.removeFirst();
    return polynomial;
}

QVector<quint8> remainder(const QVector<quint8>& data, const QVector<quint8>& divisor)
{
    QVector<quint8> result(divisor.size(), 0);
    for (const quint8 byte : data) {
        const quint8 factor = byte ^ result.first();
        result.removeFirst();
        result.append(0);
        for (int i = 0; i < result.size(); ++i) {
            result[i] ^= gfMultiply(divisor.at(i), factor);
        }
    }
    return result;
}

class Builder
{
public:
    Builder(int version, Ecc ecc)
        : m_Version(version), m_Ecc(ecc), m_Size(17 + 4 * version),
          m_Dark(m_Size * m_Size, false), m_Function(m_Size * m_Size, false)
    {
        drawFunctionPatterns();
    }

    void placeCodewords(const QVector<quint8>& codewords)
    {
        int bit = 0;
        const int totalBits = codewords.size() * 8;
        for (int right = m_Size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5; // skip the vertical timing column
            for (int step = 0; step < m_Size; ++step) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? m_Size - 1 - step : step;
                    if (!isFunction(x, y) && bit < totalBits) {
                        set(x, y, ((codewords.at(bit >> 3) >> (7 - (bit & 7))) & 1) != 0);
                        ++bit;
                    }
                    // Remainder bits (if any) stay light, as initialised.
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < m_Size; ++y) {
            for (int x = 0; x < m_Size; ++x) {
                if (isFunction(x, y)) continue;
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                default: break;
                }
                if (invert) m_Dark[y * m_Size + x] = !m_Dark[y * m_Size + x];
            }
        }
    }

    void drawFormatBits(int mask)
    {
        const int data = FormatLevelBits[level(m_Ecc)] << 3 | mask;
        int remainderBits = data;
        for (int i = 0; i < 10; ++i) remainderBits = (remainderBits << 1) ^ ((remainderBits >> 9) * 0x537);
        const int bits = (data << 10 | remainderBits) ^ 0x5412;
        auto bitAt = [bits](int i) { return ((bits >> i) & 1) != 0; };

        // Around the top-left finder.
        for (int i = 0; i <= 5; ++i) setFunction(8, i, bitAt(i));
        setFunction(8, 7, bitAt(6));
        setFunction(8, 8, bitAt(7));
        setFunction(7, 8, bitAt(8));
        for (int i = 9; i < 15; ++i) setFunction(14 - i, 8, bitAt(i));
        // Split between the top-right and bottom-left finders.
        for (int i = 0; i < 8; ++i) setFunction(m_Size - 1 - i, 8, bitAt(i));
        for (int i = 8; i < 15; ++i) setFunction(8, m_Size - 15 + i, bitAt(i));
        setFunction(8, m_Size - 8, true); // the dark module
    }

    int penalty() const
    {
        int result = 0;
        // N1 and N3 along rows and columns.
        for (int line = 0; line < m_Size; ++line) {
            result += linePenalty([&](int i) { return dark(i, line); });
            result += linePenalty([&](int i) { return dark(line, i); });
        }
        // N2: every 2x2 block of one colour.
        for (int y = 0; y < m_Size - 1; ++y) {
            for (int x = 0; x < m_Size - 1; ++x) {
                const bool colour = dark(x, y);
                if (colour == dark(x + 1, y) && colour == dark(x, y + 1) && colour == dark(x + 1, y + 1)) {
                    result += 3;
                }
            }
        }
        // N4: 10 points per full 5 % the dark share deviates from 50 %.
        int darkCount = 0;
        for (const bool module : m_Dark) darkCount += module ? 1 : 0;
        const int total = m_Size * m_Size;
        const int deviation = std::abs(darkCount * 100 - total * 50);
        result += deviation / (total * 5) * 10;
        return result;
    }

    Matrix result(int mask) const
    {
        Matrix matrix;
        matrix.version = m_Version;
        matrix.size = m_Size;
        matrix.mask = mask;
        matrix.ecc = m_Ecc;
        matrix.modules = m_Dark;
        return matrix;
    }

private:
    bool dark(int x, int y) const { return m_Dark.at(y * m_Size + x); }
    bool isFunction(int x, int y) const { return m_Function.at(y * m_Size + x); }
    void set(int x, int y, bool value) { m_Dark[y * m_Size + x] = value; }
    void setFunction(int x, int y, bool value)
    {
        m_Dark[y * m_Size + x] = value;
        m_Function[y * m_Size + x] = true;
    }

    template <typename Module>
    int linePenalty(Module module) const
    {
        int result = 0;
        // N1: a run of five or more modules of one colour.
        int runLength = 1;
        for (int i = 1; i <= m_Size; ++i) {
            if (i < m_Size && module(i) == module(i - 1)) {
                ++runLength;
                continue;
            }
            if (runLength >= 5) result += 3 + (runLength - 5);
            runLength = 1;
        }
        // N3: 1:1:3:1:1 (dark-light-dark-dark-dark-light-dark) with four light
        // modules before or after it; outside the symbol counts as light.
        static const bool Finder[7] = {true, false, true, true, true, false, true};
        auto at = [&](int i) { return i >= 0 && i < m_Size && module(i); };
        for (int start = 0; start + 7 <= m_Size; ++start) {
            bool match = true;
            for (int k = 0; k < 7 && match; ++k) match = at(start + k) == Finder[k];
            if (!match) continue;
            bool lightBefore = true;
            bool lightAfter = true;
            for (int k = 1; k <= 4; ++k) {
                lightBefore = lightBefore && !at(start - k);
                lightAfter = lightAfter && !at(start + 6 + k);
            }
            if (lightBefore) result += 40;
            if (lightAfter) result += 40;
        }
        return result;
    }

    void drawFunctionPatterns()
    {
        for (int i = 0; i < m_Size; ++i) {
            setFunction(6, i, i % 2 == 0);
            setFunction(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(m_Size - 4, 3);
        drawFinder(3, m_Size - 4);

        const QVector<int> positions = alignmentPositions(m_Version, m_Size);
        const int count = positions.size();
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < count; ++j) {
                const bool finderCorner = (i == 0 && j == 0) || (i == 0 && j == count - 1) ||
                        (i == count - 1 && j == 0);
                if (!finderCorner) drawAlignment(positions.at(i), positions.at(j));
            }
        }

        drawFormatBits(0); // reserves the area; redrawn per mask
        drawVersion();
    }

    void drawFinder(int centerX, int centerY)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = centerX + dx;
                const int y = centerY + dy;
                if (x < 0 || y < 0 || x >= m_Size || y >= m_Size) continue;
                const int distance = qMax(std::abs(dx), std::abs(dy));
                setFunction(x, y, distance != 2 && distance != 4);
            }
        }
    }

    void drawAlignment(int centerX, int centerY)
    {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                setFunction(centerX + dx, centerY + dy, qMax(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }

    void drawVersion()
    {
        if (m_Version < 7) return;
        int remainderBits = m_Version;
        for (int i = 0; i < 12; ++i) remainderBits = (remainderBits << 1) ^ ((remainderBits >> 11) * 0x1f25);
        const long bits = static_cast<long>(m_Version) << 12 | remainderBits;
        for (int i = 0; i < 18; ++i) {
            const bool bit = ((bits >> i) & 1) != 0;
            const int a = m_Size - 11 + i % 3;
            const int b = i / 3;
            setFunction(a, b, bit);
            setFunction(b, a, bit);
        }
    }

    int m_Version;
    Ecc m_Ecc;
    int m_Size;
    QVector<bool> m_Dark;
    QVector<bool> m_Function;
};

QVector<quint8> dataBits(const QByteArray& data, int version, Ecc ecc)
{
    QVector<bool> bits;
    auto append = [&bits](quint32 value, int length) {
        for (int i = length - 1; i >= 0; --i) bits.append(((value >> i) & 1) != 0);
    };
    append(0x4, 4); // byte mode
    append(static_cast<quint32>(data.size()), characterCountBits(version));
    for (const char byte : data) append(static_cast<quint8>(byte), 8);

    const int capacityBits = dataCodewords(version, ecc) * 8;
    append(0, qMin(4, capacityBits - bits.size()));      // terminator
    append(0, (8 - bits.size() % 8) % 8);                 // byte boundary
    QVector<quint8> bytes;
    for (int i = 0; i < bits.size(); i += 8) {
        quint8 byte = 0;
        for (int k = 0; k < 8; ++k) byte = static_cast<quint8>(byte << 1 | (bits.at(i + k) ? 1 : 0));
        bytes.append(byte);
    }
    for (quint8 pad = 0xec; bytes.size() < capacityBits / 8; pad ^= 0xec ^ 0x11) bytes.append(pad);
    return bytes;
}

// Splits the data into blocks, appends each block's error correction and
// interleaves them in the order the symbol stores codewords.
QVector<quint8> interleavedCodewords(const QVector<quint8>& data, int version, Ecc ecc)
{
    const int blocks = ErrorCorrectionBlocks[level(ecc)][version];
    const int eccLength = EccCodewordsPerBlock[level(ecc)][version];
    const int rawCodewords = rawDataModules(version) / 8;
    const int shortBlocks = blocks - rawCodewords % blocks;
    const int shortBlockLength = rawCodewords / blocks;
    const QVector<quint8> divisor = generator(eccLength);

    QVector<QVector<quint8>> dataBlocks;
    QVector<QVector<quint8>> eccBlocks;
    int offset = 0;
    for (int i = 0; i < blocks; ++i) {
        const int length = shortBlockLength - eccLength + (i < shortBlocks ? 0 : 1);
        const QVector<quint8> block = data.mid(offset, length);
        offset += length;
        dataBlocks.append(block);
        eccBlocks.append(remainder(block, divisor));
    }

    QVector<quint8> result;
    result.reserve(rawCodewords);
    for (int i = 0; i <= shortBlockLength - eccLength; ++i) {
        for (const QVector<quint8>& block : dataBlocks) {
            if (i < block.size()) result.append(block.at(i));
        }
    }
    for (int i = 0; i < eccLength; ++i) {
        for (const QVector<quint8>& block : eccBlocks) result.append(block.at(i));
    }
    return result;
}

}

int byteCapacity(int version, Ecc ecc)
{
    if (version < 1 || version > 40) return 0;
    const int bits = dataCodewords(version, ecc) * 8 - 4 - characterCountBits(version);
    return qMax(0, bits / 8);
}

Matrix encode(const QByteArray& data, Ecc ecc, int mask, int minimumVersion, int maximumVersion)
{
    if (mask < -1 || mask > 7 || minimumVersion < 1 || maximumVersion > 40 || minimumVersion > maximumVersion ||
            level(ecc) < 0 || level(ecc) > 3) {
        return {};
    }
    int version = minimumVersion;
    while (version <= maximumVersion && data.size() > byteCapacity(version, ecc)) ++version;
    if (version > maximumVersion) return {};

    const QVector<quint8> codewords = interleavedCodewords(dataBits(data, version, ecc), version, ecc);
    Builder base(version, ecc);
    base.placeCodewords(codewords);

    int bestMask = mask;
    if (bestMask < 0) {
        int bestPenalty = std::numeric_limits<int>::max();
        for (int candidate = 0; candidate < 8; ++candidate) {
            Builder trial = base;
            trial.applyMask(candidate);
            trial.drawFormatBits(candidate);
            const int penalty = trial.penalty();
            if (penalty < bestPenalty) {
                bestPenalty = penalty;
                bestMask = candidate;
            }
        }
    }
    Builder chosen = base;
    chosen.applyMask(bestMask);
    chosen.drawFormatBits(bestMask);
    return chosen.result(bestMask);
}

}
