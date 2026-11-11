// The data files are not built in (SKQE_EMBED_DATA=OFF, the default for local
// builds): skqe reads them from SKQE_DATA or next to the exe.
#include <cstddef>
#include <string>

namespace skqe {

bool EmbeddedDataFile(const std::wstring&, const unsigned char**, size_t*) { return false; }

}  // namespace skqe
