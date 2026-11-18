#pragma once
#include "native_tiles.hpp"

namespace nt {
// 发布前只读校验统计；失败抛 std::runtime_error，不修改任何资产。
struct OutputValidationStats {
    std::uint64_t tilesets = 0, tiles = 0, contents = 0, images = 0;
};
// 校验一个当前格式的 tileset 文档；documentPath 用于定位相对 URI 与错误消息。
// 对外开放仅用于故障夹具，产品路径使用 validateStagedOutput。
void validateTilesetDocument(const Json& document, const fs::path& documentPath);
// 校验暂存目录的主入口、分文件清单、内容、图片、包围体与误差。
OutputValidationStats validateStagedOutput(const fs::path& staging);
}
