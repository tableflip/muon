// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <utility>

#include "atom/utility/atom_content_utility_client.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "brave/utility/importer/brave_profile_import_service.h"
#include "chrome/common/importer/profile_import.mojom.h"
#include "content/public/common/resource_usage_reporter.mojom.h"
#include "chrome/utility/extensions/extensions_handler.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/service_manager_connection.h"
#include "content/public/common/simple_connection_filter.h"
#include "content/public/utility/utility_thread.h"
#include "extensions/features/features.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_message_macros.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "net/proxy_resolution/proxy_resolver_v8.h"
#include "printing/features/features.h"
#include "services/proxy_resolver/proxy_resolver_service.h"
#include "services/proxy_resolver/public/mojom/proxy_resolver.mojom.h"
#include "services/service_manager/public/cpp/binder_registry.h"
#include "services/service_manager/sandbox/switches.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "chrome/common/extensions/chrome_extensions_client.h"
#include "extensions/utility/utility_handler.h"
#endif

#if BUILDFLAG(ENABLE_PRINT_PREVIEW) || \
    (BUILDFLAG(ENABLE_BASIC_PRINTING) && defined(OS_WIN))
#include "chrome/utility/printing_handler.h"
#endif

namespace atom {

namespace {

class ResourceUsageReporterImpl : public chrome::mojom::ResourceUsageReporter {
 public:
  ResourceUsageReporterImpl() {}
  ~ResourceUsageReporterImpl() override {}

 private:
  void GetUsageData(const GetUsageDataCallback& callback) override {
    chrome::mojom::ResourceUsageDataPtr data =
      chrome::mojom::ResourceUsageData::New();
    size_t total_heap_size = net::ProxyResolverV8::GetTotalHeapSize();
    if (total_heap_size) {
      data->reports_v8_stats = true;
      data->v8_bytes_allocated = total_heap_size;
      data->v8_bytes_used = net::ProxyResolverV8::GetUsedHeapSize();
    }
    callback.Run(std::move(data));
  }
};

void CreateResourceUsageReporter(
    mojo::InterfaceRequest<chrome::mojom::ResourceUsageReporter> request) {
  mojo::MakeStrongBinding(base::MakeUnique<ResourceUsageReporterImpl>(),
                          std::move(request));
}

}  // namespace

AtomContentUtilityClient::AtomContentUtilityClient()
    : utility_process_running_elevated_(false) {
#if BUILDFLAG(ENABLE_PRINT_PREVIEW) || \
    (BUILDFLAG(ENABLE_BASIC_PRINTING) && defined(OS_WIN))
  handlers_.push_back(base::MakeUnique<printing::PrintingHandler>());
#endif
}

AtomContentUtilityClient::~AtomContentUtilityClient() = default;

void AtomContentUtilityClient::UtilityThreadStarted() {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  extensions::utility_handler::UtilityThreadStarted();
#endif

#if defined(OS_WIN)
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  utility_process_running_elevated_ = command_line->HasSwitch(
      service_manager::switches::kNoSandboxAndElevatedPrivileges);
#endif

  content::ServiceManagerConnection* connection =
      content::ChildThread::Get()->GetServiceManagerConnection();

  // NOTE: Some utility process instances are not connected to the Service
  // Manager. Nothing left to do in that case.
  if (!connection)
    return;

  auto registry = base::MakeUnique<service_manager::BinderRegistry>();
  // If our process runs with elevated privileges, only add elevated Mojo
  // interfaces to the interface registry.
  if (!utility_process_running_elevated_) {
    registry->AddInterface(base::Bind(CreateResourceUsageReporter),
                           base::ThreadTaskRunnerHandle::Get());
  }

  connection->AddConnectionFilter(
      base::MakeUnique<content::SimpleConnectionFilter>(std::move(registry)));
}

bool AtomContentUtilityClient::OnMessageReceived(
    const IPC::Message& message) {
  if (utility_process_running_elevated_)
    return false;

  for (const auto& handler : handlers_) {
    if (handler->OnMessageReceived(message))
      return true;
  }

  return false;
}

void AtomContentUtilityClient::RegisterServices(
    AtomContentUtilityClient::StaticServiceMap* services) {
  service_manager::EmbeddedServiceInfo proxy_resolver_info;
  proxy_resolver_info.task_runner =
      content::ChildThread::Get()->GetIOTaskRunner();
  proxy_resolver_info.factory =
      base::Bind(&proxy_resolver::ProxyResolverService::CreateService);
  services->emplace(proxy_resolver::mojom::kProxyResolverServiceName,
                    proxy_resolver_info);

  service_manager::EmbeddedServiceInfo profile_import_info;
  profile_import_info.factory =
    base::Bind(&BraveProfileImportService::CreateService);
  services->emplace(chrome::mojom::kProfileImportServiceName,
                    profile_import_info);
}

// static
void AtomContentUtilityClient::PreSandboxStartup() {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  extensions::ExtensionsClient::Set(
      extensions::ChromeExtensionsClient::GetInstance());
#endif
}

}  // namespace atom
