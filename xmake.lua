set_xmakever("3.0.0")
set_project("clice")

set_allowedplats("windows", "linux", "macosx")
set_allowedmodes("debug", "release", "releasedbg")

option("enable_test", { default = true })
option("dev", { default = true })
option("release", { default = false })
option("llvm", { default = nil, description = "Specify pre-compiled llvm binary directory." })
option("ci", { default = false })

if has_config("dev") then
	set_policy("build.ccache", true)
	set_policy("package.install_only", true) -- Don't fetch system package
	if is_plat("windows") then
		set_runtimes("MD")
		if is_mode("debug") then
			print(
				"clice does not support build in debug mode with pre-compiled llvm binary on windows.\n"
					.. "See https://github.com/clice-io/clice/issues/42 for more information."
			)
			os.raise()
		end
	elseif is_mode("debug") and is_plat("linux", "macosx") then
		set_policy("build.sanitizer.address", true)
	end
end

local libuv_require = "libuv"

if has_config("release") then
	set_policy("build.optimization.lto", true)
	set_policy("package.cmake_generator.ninja", true)
	set_policy("package.install_only", true) -- Don't fetch system package

	if is_plat("windows") then
		set_runtimes("MD")
		-- workaround cmake
		libuv_require = "libuv[toolchains=clang-cl]"
	end

	includes("@builtin/xpack")
end

if is_plat("macosx", "linux") then
	local cxflags
	if is_plat("macosx") then
		-- https://conda-forge.org/docs/maintainer/knowledge_base/#newer-c-features-with-old-sdk
		cxflags = "-D_LIBCPP_DISABLE_AVAILABILITY=1"
	end
	local ldflags = "-fuse-ld=lld"
	local shflags = "-fuse-ld=lld"

	add_cxflags(cxflags)
	add_ldflags(ldflags)
	add_shflags(shflags)

	add_requireconfs("**|cmake", {
		configs = {
			cxflags = cxflags,
			ldflags = ldflags,
			shflags = shflags,
		},
	})
end

add_defines("TOML_EXCEPTIONS=0")
add_requires(
	"spdlog",
	{ system = false, version = "1.15.3", configs = { header_only = false, std_format = true, noexcept = true } }
)
add_requires(libuv_require, "toml++", "croaring", "flatbuffers", "cpptrace")
add_requires("clice-llvm", { alias = "llvm" })

add_rules("mode.release", "mode.debug", "mode.releasedbg")
set_languages("c++23")
add_rules("clice_build_config")

target("clice-core", function()
	set_kind("$(kind)")
	add_files("src/**.cpp|Driver/*.cpp", "include/Index/schema.fbs")
	add_includedirs("include", { public = true })

	add_rules("flatbuffers.schema.gen", "clice_clang_tidy_config")
	add_packages("flatbuffers")
	add_packages("libuv", "spdlog", "toml++", "croaring", { public = true })

	if is_mode("debug") then
		add_packages("llvm", {
			public = true,
			links = {
				"LLVMSupport",
				"LLVMFrontendOpenMP",
				"LLVMOption",
				"LLVMTargetParser",
				"clangAST",
				"clangASTMatchers",
				"clangBasic",
				"clangDriver",
				"clangFormat",
				"clangFrontend",
				"clangLex",
				"clangSema",
				"clangSerialization",
				"clangTidy",
				"clangTidyUtils",
				-- ALL_CLANG_TIDY_CHECKS
				"clangTidyAndroidModule",
				"clangTidyAbseilModule",
				"clangTidyAlteraModule",
				"clangTidyBoostModule",
				"clangTidyBugproneModule",
				"clangTidyCERTModule",
				"clangTidyConcurrencyModule",
				"clangTidyCppCoreGuidelinesModule",
				"clangTidyDarwinModule",
				"clangTidyFuchsiaModule",
				"clangTidyGoogleModule",
				"clangTidyHICPPModule",
				"clangTidyLinuxKernelModule",
				"clangTidyLLVMModule",
				"clangTidyLLVMLibcModule",
				"clangTidyMiscModule",
				"clangTidyModernizeModule",
				"clangTidyObjCModule",
				"clangTidyOpenMPModule",
				"clangTidyPerformanceModule",
				"clangTidyPortabilityModule",
				"clangTidyReadabilityModule",
				"clangTidyZirconModule",
				"clangTooling",
				"clangToolingCore",
				"clangToolingInclusions",
				"clangToolingInclusionsStdlib",
				"clangToolingSyntax",
			},
		})
		on_config(function(target)
			local llvm_dynlib_dir = path.join(target:pkg("llvm"):installdir(), "lib")
			target:add("rpathdirs", llvm_dynlib_dir)
		end)
	elseif is_mode("release", "releasedbg") then
		add_packages("llvm", { public = true })
	end
end)

target("clice", function()
	set_kind("binary")
	add_files("bin/clice.cc")
	add_deps("clice-core")

	-- workaround
	-- @see https://github.com/xmake-io/xmake/issues/7029
	if is_plat("macosx") then
		set_toolset("dsymutil", "dsymutil")
	end

	on_config(function(target)
		local llvm_dir = target:dep("clice-core"):pkg("llvm"):installdir()
		target:add("installfiles", path.join(llvm_dir, "lib/clang/(**)"), { prefixdir = "lib/clang" })
	end)

	after_build(function(target)
		local res_dir = path.join(target:targetdir(), "../lib/clang")
		if not os.exists(res_dir) then
			local llvm_dir = target:dep("clice-core"):pkg("llvm"):installdir()
			os.vcp(path.join(llvm_dir, "lib/clang"), res_dir)
		end
	end)
end)

target("unit_tests", function()
	set_default(false)
	set_kind("binary")
	add_files("bin/unit_tests.cc", "tests/unit/**.cpp")
	add_includedirs(".", { public = true })

	add_packages("cpptrace")
	add_deps("clice-core")

	add_tests("default")

	after_load(function(target)
		target:set("runargs", "--test-dir=" .. path.absolute("tests/data"))
	end)
end)

target("integration_tests", function()
	set_default(false)
	set_kind("phony")

	add_deps("clice")
	add_packages("llvm")

	add_tests("default")

	on_test(function(target, opt)
		local argv = {
			"--log-cli-level=INFO",
			"-s",
			"tests/integration",
			"--executable=" .. target:dep("clice"):targetfile(),
		}
		local run_opt = { curdir = os.projectdir() }
		os.vrunv("pytest", argv, run_opt)

		return true
	end)
end)

rule("clice_clang_tidy_config", function()
	on_load(function(target)
		import("core.project.depend")

		local autogendir = path.join(target:autogendir(), "rules/clice_clang_tidy_config")
		os.mkdir(autogendir)
		target:add("includedirs", autogendir, { public = true })

		local src = path.join(os.projectdir(), "config/clang-tidy-config.h")
		depend.on_changed(function()
			os.vcp(src, path.join(autogendir, "clang-tidy-config.h"))
		end, {
			files = src,
			changed = target:is_rebuilt(),
		})
	end)

	before_build(function(target)
		local dest = path.join(target:autogendir(), "rules/clice_clang_tidy_config/clang-tidy-config.h")
		if not os.exists(dest) then
			local src = path.join(os.projectdir(), "config/clang-tidy-config.h")
			os.vcp(src, dest)
		end
	end)
end)

rule("clice_build_config", function()
	on_load(function(target)
		target:set("exceptions", "no-cxx")
		target:add("cxflags", "-fno-rtti", "-Wno-undefined-inline", { tools = { "clang", "clangxx", "gcc", "gxx" } })
		target:add("cxflags", "/GR-", "/Zc:preprocessor", { tools = { "clang_cl", "cl" } })

		if target:is_plat("windows") and not target:toolchain("msvc") then
			target:set("toolset", "ar", "llvm-ar")
			if target:toolchain("clang-cl") then
				target:set("toolset", "ld", "lld-link")
				target:set("toolset", "sh", "lld-link")
			else
				target:add("ldflags", "-fuse-ld=lld-link")
			end
		elseif target:is_plat("linux") then
			target:add("ldflags", "-fuse-ld=lld", "-static-libstdc++", "-Wl,--gc-sections")
		elseif target:is_plat("macosx") then
			target:add("ldflags", "-fuse-ld=lld", "-Wl,-dead_strip,-object_path_lto,clice.lto.o", { force = true })
			-- dsymutil so slow, disable it in dev ci
			if not has_config("release") and is_mode("releasedbg") and has_config("ci") then
				target:rule_enable("utils.symbols.extract", false)
			end
		end

		if has_config("release") then
			-- pixi clang failed to add lto flags because it need `-fuse-ld=lld`
			target:add("ldflags", "-flto=thin", { force = true })
		end

		if has_config("ci") then
			target:add("cxxflags", "-DCLICE_CI_ENVIRONMENT=1")
		end

		if has_config("enable_test") then
			target:add("cxxflags", "-DCLICE_ENABLE_TEST=1")
		end
	end)
end)

rule("flatbuffers.schema.gen", function()
	set_extensions(".fbs")

	on_prepare_files(function(target, jobgraph, sourcebatch, opt)
		import("lib.detect.find_tool")
		import("core.project.depend")
		import("utils.progress")

		assert(
			target:pkg("flatbuffers"),
			'Please configure add_packages("flatbuffers") for target(' .. target:name() .. ")"
		)
		local envs = target:pkgenvs()
		local flatc = assert(find_tool("flatc", { envs = envs }), "flatc not found!")

		local group_name = path.join(target:fullname(), "generate/fbs")
		local autogendir = path.join(target:autogendir(), "rules/flatbuffers")
		jobgraph:group(group_name, function()
			for _, sourcefile in ipairs(sourcebatch.sourcefiles) do
				local job = path.join(group_name, sourcefile)
				local generate_dir = path.normalize(path.join(autogendir, path.directory(sourcefile)))
				target:add("includedirs", generate_dir, { public = true })
				os.mkdir(generate_dir)
				jobgraph:add(job, function(index, total, opt)
					local argv = {
						"--cpp",
						"-o",
						generate_dir,
						sourcefile,
					}

					depend.on_changed(function()
						progress.show(flatc.progress or 0, "${color.build.object}generating.fbs %s", sourcefile)
						os.vrunv(flatc.program, argv)
					end, {
						files = sourcefile,
						dependfile = target:dependfile(sourcefile),
						changed = target:is_rebuilt(),
					})
				end)
			end
		end)
	end, { jobgraph = true })
end)

package("clice-llvm", function()
	if has_config("llvm") then
		set_sourcedir(get_config("llvm"))
	else
		on_source(function(package)
			import("core.base.json")

			local build_type = {
				Debug = "debug",
				Release = "release",
				RelWithDebInfo = "releasedbg",
			}
			local info = json.loadfile("./config/llvm-manifest.json")
			for _, info in ipairs(info) do
				local current_plat = get_config("plat")
				local current_mode = get_config("mode")
				local info_plat = info.platform:lower()
				local info_mode = build_type[info.build_type]
				local mode_match = (info_mode == current_mode)
					or (info_mode == "releasedbg" and current_mode == "release")
				if info_plat == current_plat and mode_match and (info.lto == has_config("release")) then
					package:add(
						"urls",
						format(
							"https://github.com/clice-io/clice-llvm/releases/download/%s/%s",
							info.version,
							info.filename
						)
					)
					package:add("versions", info.version, info.sha256)
				end
			end
		end)
	end

	if is_plat("linux", "macosx") then
		if is_mode("debug") then
			add_configs(
				"shared",
				{ description = "Build shared library.", default = true, type = "boolean", readonly = true }
			)
		end
	end

	if is_plat("windows", "mingw") then
		add_syslinks("version", "ntdll")
	end

	on_install(function(package)
		if not package:config("shared") then
			package:add("defines", "CLANG_BUILD_STATIC")
		end

		os.trycp("bin", package:installdir())
		os.vcp("lib", package:installdir())
		os.vcp("include", package:installdir())
	end)
end)

if has_config("release") then
	xpack("clice")
	if is_plat("windows") then
		set_formats("zip")
		set_extension(".zip")
	else
		set_formats("targz")
		set_extension(".tar.gz")
	end

	set_prefixdir("clice")

	add_targets("clice")
	-- add_installfiles(path.join(os.projectdir(), "docs/clice.toml"))

	on_package(function(package)
		import("utils.archive")

		local build_dir = path.absolute(package:install_rootdir())
		os.tryrm(build_dir)
		os.mkdir(build_dir)

		local function clice_archive(output_file)
			local old_dir = os.cd(build_dir)
			local archive_files = os.files("**")
			os.cd(old_dir)
			os.tryrm(output_file)
			cprint("packing %s .. ", output_file)
			archive.archive(path.absolute(output_file), archive_files, { curdir = build_dir, compress = "best" })
		end

		local target = package:target("clice")
		os.vcp(target:symbolfile(), build_dir)
		clice_archive(path.join(package:outputdir(), "clice-symbol" .. package:extension()))

		os.tryrm(build_dir)
		os.mkdir(path.join(build_dir, "bin"))
		os.vcp(target:targetfile(), path.join(build_dir, "bin"))
		os.vcp(path.join(os.projectdir(), "docs/clice.toml"), build_dir)

		local llvm_dir = target:dep("clice-core"):pkg("llvm"):installdir()
		os.vcp(path.join(llvm_dir, "lib/clang"), path.join(build_dir, "lib/clang"))
		clice_archive(path.join(package:outputdir(), "clice" .. package:extension()))
	end)
end
