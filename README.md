# Enfusion Unpacker

A powerful tool for viewing and extracting assets from Arma Reforger PAK files, including 3D models (XOB), textures (EDDS), and other game assets.

![Enfusion Unpacker Screenshot](https://github.com/user-attachments/assets/7a5a097f-461e-47c4-9158-78086b306d61)

## Features

### 🎮 Addon Browser
- Browse installed Workshop mods directly
- Search and filter addons by name
- View PAK file sizes and contents
- Support for both game addons and user mods

### 📁 File Browser
- Tree and list view modes
- Filter by file type (Textures, Meshes, All)
- Search functionality
- Direct navigation to assets

### 🖼️ Texture Viewer
- View EDDS textures with automatic conversion
- Support for DXT1, DXT5, BC7, and other formats
- Mipmap level viewing
- Export to PNG/DDS

### 🎨 3D Model Viewer
- Real-time 3D preview of XOB meshes
- Wireframe and solid rendering modes
- Material and texture display
- Camera controls (orbit, pan, zoom)
- Multiple view angles (Front, Back, Left, Right, Top, Bottom)
- LOD visualization
- Automatic texture loading from game PAKs

### 📝 Text Viewer
- View configuration files
- Syntax highlighting for common formats

### 📦 Batch Extraction
- Extract entire addons or filtered selections
- Convert textures to PNG on export
- Convert meshes to OBJ format
- Progress tracking with cancel support

## Installation

### Pre-built Binaries
Download the latest release from the [Releases](https://github.com/your-repo/Enfusion-Unpacker/releases) page.

### Building from Source

#### Requirements
- CMake 3.20+
- Visual Studio 2022 (or compatible C++20 compiler)
- vcpkg package manager

#### Dependencies (managed by vcpkg)
- GLFW3 (windowing)
- GLAD (OpenGL loader)
- GLM (math library)
- ImGui (GUI framework)
- LZ4 (compression)
- zlib (compression)
- SQLite3 (file indexing)
- stb_image (image loading)

#### Build Steps

```bash
# Clone the repository
git clone https://github.com/your-repo/Enfusion-Unpacker.git
cd Enfusion-Unpacker

# Configure with CMake (using vcpkg)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# Or use the build script
build.bat
```

## Usage

### GUI Mode
Simply run `EnfusionUnpacker.exe` to launch the graphical interface.

1. **Select Addon Source**: Use the dropdown to choose between Workshop Mods, Game Addons, or browse custom paths
2. **Browse Files**: Navigate the file tree to find assets
3. **Preview**: Click on files to preview them in the appropriate viewer
4. **Export**: Right-click for context menu or use File → Export

### CLI Mode
```bash
# Show help
EnfusionUnpacker.exe --help

# List files in a PAK
EnfusionUnpacker.exe --list "path/to/addon.pak"

# Extract all files
EnfusionUnpacker.exe --extract "addon.pak" --output "C:\Extracted"

# Extract with filter
EnfusionUnpacker.exe -e "addon.pak" -o "./output" -f "*.edds"

# Extract with debug logging
EnfusionUnpacker.exe --debug -e "addon.pak" -o "./output"
```

#### CLI Options
| Option | Short | Description |
|--------|-------|-------------|
| `--help` | `-h` | Show help message |
| `--list` | `-l` | List files in PAK |
| `--extract` | `-e` | Extract files from PAK |
| `--output` | `-o` | Output directory |
| `--filter` | `-f` | Filter pattern (glob-style) |
| `--verbose` | `-v` | Verbose output |
| `--debug` | `-d` | Enable debug logging |

## Configuration

### Settings (GUI)
Access via **Tools → Settings** or press `Ctrl+,`

- **General**: Default paths, startup options
- **Appearance**: Theme selection, UI scale
- **Export**: Default conversion options
- **Paths**: Game and mod directories
- **Logging**: Log level and file output

### Paths
The application automatically detects:
- **Arma Reforger**: `C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger`
- **Workshop Mods**: `C:\Users\[User]\Documents\My Games\ArmaReforger\addons`

Custom paths can be configured in Settings.

## File Format Support

| Format | Extension | Support |
|--------|-----------|---------|
| PAK Archive | `.pak` | ✅ Full |
| Texture | `.edds` | ✅ View/Convert to PNG |
| 3D Model | `.xob` | ✅ View/Convert to OBJ |
| Material | `.emat` | ✅ Parse for textures |
| Config | `.conf` | ✅ View |
| Layout | `.layout` | ✅ View |
| Script | `.c` | ✅ View |

## Troubleshooting

### Enable Debug Logging
1. **GUI**: Settings → Logging → Set level to "Debug"
2. **CLI**: Add `--debug` flag

### Log File Location
Logs are written to `enfusion_unpacker.log` in the application directory.

### Common Issues

**Model appears without textures**
- Textures are searched across ALL indexed PAKs (game + mods)
- Ensure the game path is correctly configured in Settings
- The SQLite index must contain game base PAKs for shared textures
- Check logs for texture search attempts and results
- Some materials are procedural (chrome, mirror) and have no texture files

**PAK fails to open**
- Verify the file is a valid Arma Reforger PAK
- Check if the file is corrupted or incomplete
- Look for error messages in the log

**Slow performance with large addons**
- The file index is cached in SQLite for faster subsequent loads
- First load may take longer while indexing
- Subsequent texture searches across all PAKs are fast due to SQL indexing

### Global Texture Index
The application builds a unified index of ALL files across:
- Game base PAKs (~300+ files)
- Workshop mod PAKs
- User addon PAKs

This allows textures to be found even when they're in game base PAKs but referenced by mod models. The index contains **~400,000 files across ~400+ PAKs** and uses SQLite for instant lookups.

## Technical Details

### PAK Format
Arma Reforger uses a custom PAK format with:
- ZSTD/LZ4 compressed chunks
- FILE chunk containing file entries
- Embedded file data with offset mapping

### XOB Format
3D models use a proprietary format with:
- HEAD chunk: Metadata and material references
- LODS chunk: Mesh geometry data
- LZ4 chained compression

### EDDS Format
Extended DDS textures with:
- Custom header format
- Support for BC1-BC7 compression
- Mipmap chains

## Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Submit a pull request

**Note**: This tool is intended for legitimate modding purposes. Please respect content creators' rights and Bohemia Interactive's terms of service.
