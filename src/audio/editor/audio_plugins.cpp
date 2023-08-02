#include <imgui/imgui.h>

#include "audio_device.h"
#include "audio_module.h"
#include "audio_system.h"
#include "clip.h"
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/editor_asset.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/engine.h"
#include "engine/lua_wrapper.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/world.h"


using namespace Lumix;


namespace
{


struct AssetBrowserPlugin final : AssetBrowser::Plugin, AssetCompiler::IPlugin {
	struct Meta {
		bool looped = true;
		float volume = 1.f;

		void load(const Path& path, StudioApp& app) {
			app.getAssetCompiler().getMeta(path, [&](lua_State* L){
				LuaWrapper::getOptionalField(L, LUA_GLOBALSINDEX, "looped", &looped);
				LuaWrapper::getOptionalField(L, LUA_GLOBALSINDEX, "volume", &volume);
			});
		}
	};

	struct EditorWindow : AssetEditorWindow {
		EditorWindow(const Path& path, StudioApp& app, IAllocator& allocator)
			: AssetEditorWindow(app)
			, m_allocator(allocator)
			, m_app(app)
		{
			m_resource = app.getEngine().getResourceManager().load<Clip>(path);
			m_meta.load(path, app);
		}

		~EditorWindow() {
			stopAudio();
			m_resource->decRefCount();
		}

		void save() {
			AssetCompiler& compiler = m_app.getAssetCompiler();
			const StaticString<512> src("looped = ", m_meta.looped ? "true" : "false"
				, "\nvolume = ", m_meta.volume
			);
			compiler.updateMeta(m_resource->getPath(), src);
			m_dirty = false;
		}
		
		bool onAction(const Action& action) override { 
			if (&action == &m_app.getSaveAction()) save();
			else return false;
			return true;
		}

		void windowGUI() override {
			if (ImGui::BeginMenuBar()) {
				if (ImGuiEx::IconButton(ICON_FA_SAVE, "Save")) save();
				if (ImGuiEx::IconButton(ICON_FA_EXTERNAL_LINK_ALT, "Open externally")) m_app.getAssetBrowser().openInExternalEditor(m_resource);
				ImGui::EndMenuBar();
			}

			if (m_resource->isEmpty()) {
				ImGui::TextUnformatted("Loading...");
				return;
			}

			ImGuiEx::Label("Looped");
			m_dirty = ImGui::Checkbox("##loop", &m_meta.looped) || m_dirty;
			ImGuiEx::Label("Volume");
			m_dirty = ImGui::DragFloat("##vol", &m_meta.volume, 0.01f, 0, FLT_MAX) || m_dirty;

			ImGuiEx::Label("Length");
			ImGui::Text("%f", m_resource->getLengthSeconds());
			auto& device = getAudioDevice(m_app.getEngine());

			if (m_playing_clip >= 0)
			{
				if (ImGui::Button(ICON_FA_STOP "Stop")) {
					stopAudio();
					return;
				}
				float time = device.getCurrentTime(m_playing_clip);
				ImGuiEx::Label("Time");
				if (ImGui::SliderFloat("##time", &time, 0, m_resource->getLengthSeconds(), "%.2fs")) {
					device.setCurrentTime(m_playing_clip, time);
				}
			}

			if (m_playing_clip < 0 && ImGui::Button(ICON_FA_PLAY "Play")) {
				stopAudio();

				AudioDevice::BufferHandle handle = device.createBuffer(m_resource->getData(), m_resource->getSize(), m_resource->getChannels(), m_resource->getSampleRate(), 0);
				if (handle != AudioDevice::INVALID_BUFFER_HANDLE) {
					device.setVolume(handle, m_resource->m_volume);
					device.play(handle, true);
					m_playing_clip = handle;
				}
			}

			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_CHECK "Apply")) {
				const StaticString<512> src("volume = ", m_meta.volume
					, "\nlooped = ", m_meta.looped ? "true" : "false"
				);
				AssetCompiler& compiler = m_app.getAssetCompiler();
				compiler.updateMeta(m_resource->getPath(), src);
			}
		}
	
		void destroy() override { LUMIX_DELETE(m_allocator, this); }
		const Path& getPath() override { return m_resource->getPath(); }
		const char* getName() const override { return "audio clip editor"; }
		
		static AudioDevice& getAudioDevice(Engine& engine) {
			auto* audio = static_cast<AudioSystem*>(engine.getSystemManager().getSystem("audio"));
			return audio->getDevice();
		}


		void stopAudio() {
			if (m_playing_clip < 0) return;

			getAudioDevice(m_app.getEngine()).stop(m_playing_clip);
			m_playing_clip = -1;
		}

		IAllocator& m_allocator;
		StudioApp& m_app;
		Clip* m_resource;
		Meta m_meta;
		i32 m_playing_clip;
	};

	explicit AssetBrowserPlugin(StudioApp& app)
		: m_app(app)
		, m_browser(app.getAssetBrowser())
	{
		app.getAssetCompiler().registerExtension("ogg", Clip::TYPE);
		app.getAssetCompiler().registerExtension("wav", Clip::TYPE);
	}
	
	void onResourceDoubleClicked(const Path& path) override {
		AssetBrowser& ab = m_app.getAssetBrowser();
		if (AssetEditorWindow* win = ab.getWindow(Path(path))) {
			win->m_focus_request = true;
			return;
		}
	
		IAllocator& allocator = m_app.getAllocator();
		EditorWindow* win = LUMIX_NEW(allocator, EditorWindow)(Path(path), m_app, m_app.getAllocator());
		ab.addWindow(win);
	}

	bool compile(const Path& src) override {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		OutputMemoryStream src_data(m_app.getAllocator());
		if (!fs.getContentSync(src, src_data)) return false;
		
		Meta meta;
		meta.load(src, m_app);

		OutputMemoryStream compiled(m_app.getAllocator());
		compiled.reserve(64 + src_data.size());
		compiled.write((u32)0);
		const bool is_wav = Path::hasExtension(src.c_str(), "wav");
		compiled.write(is_wav ? Clip::Format::WAV : Clip::Format::OGG);
		compiled.write(meta.looped);
		compiled.write(meta.volume);
		compiled.write(src_data.data(), src_data.size());
		return m_app.getAssetCompiler().writeCompiledResource(src.c_str(), Span(compiled.data(), (i32)compiled.size()));
	}

	const char* getName() const override { return "Audio"; }
	ResourceType getResourceType() const override { return Clip::TYPE; }
	
	StudioApp& m_app;
	AssetBrowser& m_browser;
};


struct StudioAppPlugin : StudioApp::IPlugin
{
	explicit StudioAppPlugin(StudioApp& app)
		: m_app(app)
		, m_asset_browser_plugin(app)
	{}

	const char* getName() const override { return "audio"; }

	void init() override 
	{
		m_app.getAssetBrowser().addPlugin(m_asset_browser_plugin);
		const char* extensions[] = { "ogg", "wav", nullptr };
		m_app.getAssetCompiler().addPlugin(m_asset_browser_plugin, extensions);
	}


	bool showGizmo(WorldView& view, ComponentUID cmp) override
	{
		static const ComponentType ECHO_ZONE_TYPE = reflection::getComponentType("echo_zone");
		static const ComponentType CHORUS_ZONE_TYPE = reflection::getComponentType("chorus_zone");

		const EntityRef entity = (EntityRef)cmp.entity;
		if (cmp.type == ECHO_ZONE_TYPE)
		{
			auto* audio_module = static_cast<AudioModule*>(cmp.module);
			float radius = audio_module->getEchoZone(entity).radius;
			World& world = audio_module->getWorld();
			const DVec3 pos = world.getPosition(entity);

			addSphere(view, pos, radius, Color::BLUE);
			return true;
		}
		else if (cmp.type == CHORUS_ZONE_TYPE)
		{
			auto* audio_module = static_cast<AudioModule*>(cmp.module);
			float radius = audio_module->getChorusZone(entity).radius;
			World& world = audio_module->getWorld();
			const DVec3 pos = world.getPosition(entity);

			addSphere(view, pos, radius, Color::BLUE);
			return true;
		}

		return false;
	}


	~StudioAppPlugin()
	{
		m_app.getAssetBrowser().removePlugin(m_asset_browser_plugin);
		m_app.getAssetCompiler().removePlugin(m_asset_browser_plugin);
	}


	StudioApp& m_app;
	AssetBrowserPlugin m_asset_browser_plugin;
};


} // anonymous namespace


LUMIX_STUDIO_ENTRY(audio)
{
	IAllocator& allocator = app.getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}
