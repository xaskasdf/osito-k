#include <version.h>

#include "dxvk_instance.h"
#include "dxvk_openvr.h"
#include "dxvk_openxr.h"
#include "dxvk_platform_exts.h"
#include "../wsi/wsi_platform.h"

#include <algorithm>
#include <sstream>

extern "C" int printf(const char*, ...);

namespace dxvk {
  
  DxvkInstance::DxvkInstance(DxvkInstanceFlags flags)
  : DxvkInstance(DxvkInstanceImportInfo(), flags) {

  }


  DxvkInstance::DxvkInstance(const DxvkInstanceImportInfo& args, DxvkInstanceFlags flags) {
    printf("[DXVKinst] ctor begin flags=0x%x imported=%d\n",
      flags.raw(), args.instance != VK_NULL_HANDLE);
    Logger::info(str::format("Game: ", env::getExeName()));
    Logger::info(str::format("DXVK: ", DXVK_VERSION));

    printf("[DXVKinst] wsi init begin\n");
    wsi::init();
    printf("[DXVKinst] wsi init done\n");

    printf("[DXVKinst] config begin\n");
    m_config = Config::getUserConfig();
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_config.logOptions();

    m_options = DxvkOptions(m_config);
    printf("[DXVKinst] config done\n");

    // Load Vulkan library
    printf("[DXVKinst] createLibraryLoader begin\n");
    createLibraryLoader(args);
    printf("[DXVKinst] createLibraryLoader done valid=%d\n", (int)m_vkl->valid());

    if (!m_vkl->valid())
      dxvk::DxvkError::abort_ositok("Failed to load vulkan-1 library.");

    // Initialize extension providers
    m_extProviders.push_back(&DxvkPlatformExts::s_instance);
#ifdef _WIN32
    m_extProviders.push_back(&VrInstance::s_instance);
    m_extProviders.push_back(&DxvkXrProvider::s_instance);
#endif

    Logger::info("Built-in extension providers:");
    for (const auto& provider : m_extProviders)
      Logger::info(str::format("  ", provider->getName()));

    printf("[DXVKinst] init instance ext providers begin\n");
    for (const auto& provider : m_extProviders)
      provider->initInstanceExtensions();
    printf("[DXVKinst] init instance ext providers done\n");

    printf("[DXVKinst] createInstanceLoader begin\n");
    createInstanceLoader(args, flags);
    printf("[DXVKinst] createInstanceLoader done\n");

    printf("[DXVKinst] queryAdapters begin\n");
    m_adapters = this->queryAdapters();
    printf("[DXVKinst] queryAdapters done count=%u\n", (unsigned)m_adapters.size());

    printf("[DXVKinst] init device ext providers begin\n");
    for (const auto& provider : m_extProviders)
      provider->initDeviceExtensions(this);
    printf("[DXVKinst] init device ext providers done\n");

    printf("[DXVKinst] enable adapter extensions begin\n");
    for (uint32_t i = 0; i < m_adapters.size(); i++) {
      for (const auto& provider : m_extProviders) {
        m_adapters[i]->enableExtensions(
          provider->getDeviceExtensions(i));
      }
    }
    printf("[DXVKinst] ctor done\n");
  }
  
  
  DxvkInstance::~DxvkInstance() {
    if (m_messenger)
      m_vki->vkDestroyDebugUtilsMessengerEXT(m_vki->instance(), m_messenger, nullptr);

    wsi::quit();
  }
  
  
  Rc<DxvkAdapter> DxvkInstance::enumAdapters(uint32_t index) const {
    return index < m_adapters.size()
      ? m_adapters[index]
      : nullptr;
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByLuid(const void* luid) const {
    for (const auto& adapter : m_adapters) {
      const auto& vk11 = adapter->devicePropertiesExt().vk11;

      if (vk11.deviceLUIDValid && !std::memcmp(luid, vk11.deviceLUID, VK_LUID_SIZE))
        return adapter;
    }

    return nullptr;
  }

  
  Rc<DxvkAdapter> DxvkInstance::findAdapterByDeviceId(uint16_t vendorId, uint16_t deviceId) const {
    for (const auto& adapter : m_adapters) {
      const auto& props = adapter->deviceProperties();

      if (props.vendorID == vendorId
       && props.deviceID == deviceId)
        return adapter;
    }

    return nullptr;
  }
  
  
  void DxvkInstance::createLibraryLoader(const DxvkInstanceImportInfo& args) {
    m_vkl = args.loaderProc
      ? new vk::LibraryFn(args.loaderProc)
      : new vk::LibraryFn();
  }


  void DxvkInstance::createInstanceLoader(const DxvkInstanceImportInfo& args, DxvkInstanceFlags flags) {
    printf("[DXVKinst] CIL start imported=%d\n", args.instance != VK_NULL_HANDLE);
    DxvkNameList layerList;
    DxvkNameList extensionList;
    DxvkNameSet extensionSet;

    bool enablePerfEvents = false;
    bool enableValidation = false;

    if (args.instance) {
      extensionList = DxvkNameList(args.extensionCount, args.extensionNames);
      extensionSet = getExtensionSet(extensionList);

      auto extensionInfos = getExtensionList(m_extensions, true);

      if (!extensionSet.enableExtensions(extensionInfos.size(), extensionInfos.data(), nullptr))
        dxvk::DxvkError::abort_ositok("DxvkInstance: Required instance extensions not enabled");
    } else {
      // Hide VK_EXT_debug_utils behind an environment variable.
      // This extension adds additional overhead to winevulkan.
      std::string debugEnv = env::getEnvVar("DXVK_DEBUG");

      enablePerfEvents = debugEnv == "markers";
      enableValidation = debugEnv == "validation";

      bool enableDebug = enablePerfEvents || enableValidation || m_options.enableDebugUtils;

      if (enableDebug) {
        Logger::warn("Debug Utils are enabled. May affect performance.");

        if (enableValidation) {
          const char* layerName = "VK_LAYER_KHRONOS_validation";
          DxvkNameSet layers = DxvkNameSet::enumInstanceLayers(m_vkl);

          if (layers.supports(layerName)) {
            layerList.add(layerName);
            Logger::warn(str::format("Enabled instance layer ", layerName));
          } else {
            // This can happen on winevulkan since it does not support layers
            Logger::warn(str::format("Validation layers not found, set VK_INSTANCE_LAYERS=", layerName));
          }
        }
      }

      // Get set of extensions to enable based on available
      // extensions and extension providers.
      printf("[DXVKinst] CIL getExtensionList begin\n");
      auto extensionInfos = getExtensionList(m_extensions, enableDebug);
      printf("[DXVKinst] CIL enumInstanceExtensions begin\n");
      DxvkNameSet extensionsAvailable = DxvkNameSet::enumInstanceExtensions(m_vkl);
      printf("[DXVKinst] CIL enumInstanceExtensions done\n");

      printf("[DXVKinst] CIL enableExtensions begin count=%u\n", (unsigned)extensionInfos.size());
      if (!extensionsAvailable.enableExtensions(extensionInfos.size(), extensionInfos.data(), &extensionSet))
        dxvk::DxvkError::abort_ositok("DxvkInstance: Required instance extensions not supported");
      printf("[DXVKinst] CIL enableExtensions done\n");

      printf("[DXVKinst] CIL merge provider extensions begin\n");
      for (const auto& provider : m_extProviders)
        extensionSet.merge(provider->getInstanceExtensions());
      printf("[DXVKinst] CIL merge provider extensions done\n");

      // Generate list of extensions to enable
      extensionList = extensionSet.toNameList();
    }

    Logger::info("Enabled instance extensions:");
    this->logNameList(extensionList);

    // If necessary, create a new Vulkan instance
    VkInstance instance = args.instance;

    if (!instance) {
      std::string appName = env::getExeName();

      VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
      appInfo.pApplicationName      = appName.c_str();
      appInfo.applicationVersion    = flags.raw();
      appInfo.pEngineName           = "DXVK";
      appInfo.engineVersion         = VK_MAKE_API_VERSION(0, 2, 4, 0);
      appInfo.apiVersion            = VK_MAKE_API_VERSION(0, 1, 3, 0);

      VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
      info.pApplicationInfo         = &appInfo;
      info.enabledLayerCount        = layerList.count();
      info.ppEnabledLayerNames      = layerList.names();
      info.enabledExtensionCount    = extensionList.count();
      info.ppEnabledExtensionNames  = extensionList.names();

      printf("[DXVKinst] vkCreateInstance begin layers=%u exts=%u\n",
        layerList.count(), extensionList.count());
      VkResult status = m_vkl->vkCreateInstance(&info, nullptr, &instance);
      printf("[DXVKinst] vkCreateInstance done status=%d instance=%p\n",
        status, (void*)instance);

      if (status != VK_SUCCESS)
        dxvk::DxvkError::abort_ositok("DxvkInstance::createInstance: Failed to create Vulkan 1.1 instance");
    }

    // Create the Vulkan instance loader
    printf("[DXVKinst] new InstanceFn begin\n");
    m_vki = new vk::InstanceFn(m_vkl, !args.instance, instance);
    printf("[DXVKinst] new InstanceFn done\n");

    if (enableValidation) {
      VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
      messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      messengerInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
      messengerInfo.pfnUserCallback = &debugCallback;

      if (m_vki->vkCreateDebugUtilsMessengerEXT(m_vki->instance(), &messengerInfo, nullptr, &m_messenger))
        Logger::err("DxvkInstance::createInstance: Failed to create debug messenger, proceeding without.");
    }
  }


  std::vector<DxvkExt*> DxvkInstance::getExtensionList(DxvkInstanceExtensions& ext, bool withDebug) {
    std::vector<DxvkExt*> result = {{
      &ext.extSurfaceMaintenance1,
      &ext.khrGetSurfaceCapabilities2,
      &ext.khrSurface,
    }};

    if (withDebug)
      result.push_back(&ext.extDebugUtils);

    return result;
  }


  DxvkNameSet DxvkInstance::getExtensionSet(const DxvkNameList& extensions) {
    DxvkNameSet enabledSet(extensions.count(), extensions.names());
    enabledSet.mergeRevisions(DxvkNameSet::enumInstanceLayers(m_vkl));
    return enabledSet;
  }


  std::vector<Rc<DxvkAdapter>> DxvkInstance::queryAdapters() {
    uint32_t numAdapters = 0;
    printf("[DXVKinst] vkEnumeratePhysicalDevices count begin\n");
    if (m_vki->vkEnumeratePhysicalDevices(m_vki->instance(), &numAdapters, nullptr) != VK_SUCCESS)
      dxvk::DxvkError::abort_ositok("DxvkInstance::enumAdapters: Failed to enumerate adapters");
    printf("[DXVKinst] vkEnumeratePhysicalDevices count done n=%u\n", numAdapters);
    
    std::vector<VkPhysicalDevice> adapters(numAdapters);
    printf("[DXVKinst] vkEnumeratePhysicalDevices list begin\n");
    if (m_vki->vkEnumeratePhysicalDevices(m_vki->instance(), &numAdapters, adapters.data()) != VK_SUCCESS)
      dxvk::DxvkError::abort_ositok("DxvkInstance::enumAdapters: Failed to enumerate adapters");
    printf("[DXVKinst] vkEnumeratePhysicalDevices list done n=%u\n", numAdapters);

    std::vector<VkPhysicalDeviceProperties> deviceProperties(numAdapters);
    DxvkDeviceFilterFlags filterFlags = 0;

    for (uint32_t i = 0; i < numAdapters; i++) {
      printf("[DXVKinst] vkGetPhysicalDeviceProperties begin i=%u dev=%p\n",
        i, (void*)adapters[i]);
      m_vki->vkGetPhysicalDeviceProperties(adapters[i], &deviceProperties[i]);
      printf("[DXVKinst] vkGetPhysicalDeviceProperties done i=%u type=%u vendor=0x%x device=0x%x\n",
        i, deviceProperties[i].deviceType, deviceProperties[i].vendorID,
        deviceProperties[i].deviceID);

      if (deviceProperties[i].deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        filterFlags.set(DxvkDeviceFilterFlag::SkipCpuDevices);
    }

    DxvkDeviceFilter filter(filterFlags, m_options);
    std::vector<Rc<DxvkAdapter>> result;

    uint32_t numDGPU = 0;
    uint32_t numIGPU = 0;

    for (uint32_t i = 0; i < numAdapters; i++) {
      if (filter.testAdapter(deviceProperties[i])) {
        printf("[DXVKinst] new DxvkAdapter begin i=%u\n", i);
        result.push_back(new DxvkAdapter(m_vki, adapters[i]));
        printf("[DXVKinst] new DxvkAdapter done i=%u result=%u\n",
          i, (unsigned)result.size());

        if (deviceProperties[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
          numDGPU += 1;
        else if (deviceProperties[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
          numIGPU += 1;
      }
    }
    
    std::stable_sort(result.begin(), result.end(),
      [] (const Rc<DxvkAdapter>& a, const Rc<DxvkAdapter>& b) -> bool {
        static const std::array<VkPhysicalDeviceType, 3> deviceTypes = {{
          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
          VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
        }};

        uint32_t aRank = deviceTypes.size();
        uint32_t bRank = deviceTypes.size();

        for (uint32_t i = 0; i < std::min(aRank, bRank); i++) {
          if (a->deviceProperties().deviceType == deviceTypes[i]) aRank = i;
          if (b->deviceProperties().deviceType == deviceTypes[i]) bRank = i;
        }

        return aRank < bRank;
      });

    if (m_options.hideIntegratedGraphics && numDGPU > 0 && numIGPU > 0) {
      result.resize(numDGPU);
      numIGPU = 0;
    }

    if (result.empty()) {
      Logger::warn("DXVK: No adapters found. Please check your "
                   "device filter settings and Vulkan setup. "
                   "A Vulkan 1.3 capable driver is required.");
    } else if (numDGPU == 1 && numIGPU == 1) {
      result[1]->linkToDGPU(result[0]);
    }
    
    return result;
  }
  
  
  void DxvkInstance::logNameList(const DxvkNameList& names) {
    for (uint32_t i = 0; i < names.count(); i++)
      Logger::info(str::format("  ", names.name(i)));
  }
  

  VkBool32 VKAPI_CALL DxvkInstance::debugCallback(
          VkDebugUtilsMessageSeverityFlagBitsEXT  messageSeverity,
          VkDebugUtilsMessageTypeFlagsEXT         messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT*   pCallbackData,
          void*                                   pUserData) {
    LogLevel logLevel;

    switch (messageSeverity) {
      default:
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:    logLevel = LogLevel::Info;  break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: logLevel = LogLevel::Debug; break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: logLevel = LogLevel::Warn;  break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:   logLevel = LogLevel::Error; break;
    }

    static const std::array<uint32_t, 8> ignoredIds = {
      // Ignore image format features for depth-compare instructions.
      // These errors are expected in D3D9 and some D3D11 apps.
      0x23259a0d,
      0x4b9d1597,
      0x534c50ad,
      0x9750b479,
      // Ignore vkCmdBindPipeline errors related to dynamic rendering.
      // Validation layers are buggy here and will complain about any
      // command buffer with more than one render pass.
      0x11b37e31,
      0x151f5e5a,
      0x6c16bfb4,
      0xd6d77e1e,
    };

    for (auto id : ignoredIds) {
      if (uint32_t(pCallbackData->messageIdNumber) == id)
        return VK_FALSE;
    }

    std::stringstream str;

    if (pCallbackData->pMessageIdName)
      str << pCallbackData->pMessageIdName << ": " << std::endl;

    str << pCallbackData->pMessage;

    Logger::log(logLevel, str.str());
    return VK_FALSE;
  }

}
