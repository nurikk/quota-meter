import subprocess
from pathlib import Path
from typing import Any

globals()["Import"]("env")
env: Any = globals()["env"]

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
cmake_dir = Path(env.PioPlatform().get_package_dir("tool-cmake"))
idf_dir = Path(env.PioPlatform().get_package_dir("framework-espidf"))
cmake = cmake_dir / "bin" / "cmake"
embed_script = idf_dir / "tools" / "cmake" / "scripts" / "data_file_embed_asm.cmake"
build_dir.mkdir(parents=True, exist_ok=True)

for name in ("portal.html", "portal.js"):
    source = project_dir / "src" / "portal" / name
    output = build_dir / f"{name}.S"
    subprocess.run(
        [
            str(cmake),
            f"-DDATA_FILE={source}",
            f"-DSOURCE_FILE={output}",
            "-DFILE_TYPE=TEXT",
            "-P",
            str(embed_script),
        ],
        check=True,
    )
