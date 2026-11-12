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
                        "libraries": [
                            "-lvirt",
                            "-lvirt-qemu",
                            "-lisofs"
                        ],
		}
	]
}
