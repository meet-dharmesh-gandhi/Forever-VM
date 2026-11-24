{
	"targets": [
		{
			"target_name": "node",
			"sources": ["node.c",
                        "../include/async.c",
                        "../include/memory.c",
                        "../include/morph.c",
                        "../include/network.c",
                        "../include/print.c",
                        "../include/utils.c",
                        ],
            "include_dirs": [
                "/usr/include/libxml2",
            ],
                        # "cflags": [
                        #     "-fplugin=../../miscellaneous/Safe-C/build/lib/safe_c.so",
                        #     "-fplugin-arg-safe_c-mode=warn"
                        # ],
                        # "cflags_c": [
                        #     "-fplugin=../../miscellaneous/Safe-C/build/lib/safe_c.so",
                        #     "-fplugin-arg-safe_c-mode=warn"
                        # ],
                        "libraries": [
                            "-lvirt",
                            "-lvirt-qemu",
                            "-lisofs",
                            "-lxml2"
                        ],
		}
	]
}
