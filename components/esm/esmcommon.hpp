#ifndef OPENMW_ESM_COMMON_H
#define OPENMW_ESM_COMMON_H

#include <string>
#include <cstring>
#include <vector>
#include <string_view>
#include <cstdint>

namespace ESM
{
enum Version
  {
    VER_12 = 0x3f99999a,
    VER_13 = 0x3fa66666
  };

enum RecordFlag
  {
    FLAG_Persistent = 0x00000400,
    FLAG_Blocked    = 0x00002000
  };

template <std::size_t capacity>
struct FixedString
{
    static_assert(capacity > 0);

    static constexpr std::size_t sCapacity = capacity;

    char mData[capacity];

    std::string_view toStringView() const noexcept
    {
        return std::string_view(mData, strnlen(mData, capacity));
    }

    std::string toString() const
    {
        return std::string(toStringView());
    }

    std::uint32_t toInt() const noexcept
    {
        static_assert(capacity == sizeof(std::uint32_t));
        std::uint32_t value;
        std::memcpy(&value, mData, capacity);
        return value;
    }

    void clear() noexcept
    {
        std::memset(mData, 0, capacity);
    }

    void assign(std::string_view value) noexcept
    {
        if (value.empty())
        {
            clear();
            return;
        }

        if (value.size() < capacity)
        {
            if constexpr (capacity == sizeof(std::uint32_t))
                std::memset(mData, 0, capacity);
            std::memcpy(mData, value.data(), value.size());
            if constexpr (capacity != sizeof(std::uint32_t))
                mData[value.size()] = '\0';
            return;
        }

        std::memcpy(mData, value.data(), capacity);

        if constexpr (capacity != sizeof(std::uint32_t))
            mData[capacity - 1] = '\0';
    }

    FixedString& operator=(std::uint32_t value) noexcept
    {
        static_assert(capacity == sizeof(value));
        std::memcpy(&mData, &value, capacity);
        return *this;
    }
};

template <std::size_t capacity, class T, typename = std::enable_if_t<std::is_same_v<T, char>>>
inline bool operator==(const FixedString<capacity>& lhs, const T* const& rhs) noexcept
{
    for (std::size_t i = 0; i < capacity; ++i)
    {
        if (lhs.mData[i] != rhs[i])
            return false;
        if (lhs.mData[i] == '\0')
            return true;
    }
    return rhs[capacity] == '\0';
}

template <std::size_t capacity>
inline bool operator==(const FixedString<capacity>& lhs, const std::string& rhs) noexcept
{
    return lhs == rhs.c_str();
}

template <std::size_t capacity, std::size_t rhsSize>
inline bool operator==(const FixedString<capacity>& lhs, const char (&rhs)[rhsSize]) noexcept
{
    return strnlen(rhs, rhsSize) == strnlen(lhs.mData, capacity)
        && std::strncmp(lhs.mData, rhs, capacity) == 0;
}

inline bool operator==(const FixedString<4>& lhs, std::uint32_t rhs) noexcept
{
    return lhs.toInt() == rhs;
}

template <std::size_t capacity, class Rhs>
inline bool operator!=(const FixedString<capacity>& lhs, const Rhs& rhs) noexcept
{
    return !(lhs == rhs);
}

using NAME = FixedString<4>;
using NAME32 = FixedString<32>;
using NAME64 = FixedString<64>;

/* This struct defines a file 'context' which can be saved and later
   restored by an ESMReader instance. It will save the position within
   a file, and when restored will let you read from that position as
   if you never left it.
 */
struct ESM_Context
{
  std::string filename;
  uint32_t leftRec, leftSub;
  size_t leftFile;
  NAME recName, subName;
  // When working with multiple esX files, we will generate lists of all files that
  //  actually contribute to a specific cell. Therefore, we need to store the index
  //  of the file belonging to this contest. See CellStore::(list/load)refs for details.
  int index;
  std::vector<int> parentFileIndices;

  // True if subName has been read but not used.
  bool subCached;

  // File position. Only used for stored contexts, not regularly
  // updated within the reader itself.
  size_t filePos;
};

}

#endif
