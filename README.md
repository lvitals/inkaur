
     ___ _   _ _  __   _   _  _ ____ 
    |_ _| \ | | |/ /  /_\ | | | |  _ \
     | ||  \| | ' /  / _ \| | | | |_) |
     | || |\  | . \ / ___ \ |_| |  _ < 
    |___|_| \_|_|\_/_/   \_\___/|_| \_\


**inkaur** is a simple, lightweight, and fast AUR helper written in C. It provides a familiar `pacman`-like interface for managing packages from the Arch User Repository, focusing on minimalism and performance.

## Features

- **Sync (-S)**: Download, build, and install packages from the AUR.
- **Search (-Ss)**: Search the AUR for packages using keywords.
- **Info (-Si)**: View detailed information about a specific AUR package.
- **Update (-Syu)**: Check and update all installed AUR packages.
- **Remove (-R)**: Remove packages using pacman (supports `-Rs`, `-Rns`, etc.).
- **Query (-Q)**: List and filter installed AUR packages (supports `-Qe`, `-Qd`, `-Qdt`, `-Qq`).
- **Clean (-Sc)**: Clean the local build cache (supports `-Scc` for full removal).

## Dependencies

To build and run **inkaur**, you need:

- `libcurl`: For AUR RPC API requests.
- `libalpm`: For local database and package management.
- `git`: For cloning AUR package repositories.
- `base-devel`: Required for building packages via `makepkg`.

## Installation

```bash
git clone https://github.com/lvsantos/inkaur
cd inkaur
make
sudo make install
```

## Usage

**inkaur** follows the same command structure as `pacman`.

### Installing Packages
```bash
inkaur -S package_name
```

### Searching
```bash
inkaur -Ss keyword
```

### Updating the System (AUR only)
```bash
inkaur -Syu
```

### Package Information
```bash
inkaur -Si package_name
```

### Listing Installed AUR Packages
```bash
inkaur -Q      # List all
inkaur -Qe     # Explicitly installed
inkaur -Qdt    # Unused dependencies (orphans)
```

### Removing Packages
```bash
inkaur -Rns package_name
```

### Cleaning Cache
```bash
inkaur -Sc     # Remove built packages
inkaur -Scc    # Remove entire cache directory
```

## Cache Location
The build cache is located at `~/.cache/inkaur`.

## License
MIT License.
