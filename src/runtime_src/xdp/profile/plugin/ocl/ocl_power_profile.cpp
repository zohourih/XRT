# include "xdp/profile/plugin/ocl/ocl_power_profile.h"

namespace xdp {

OclPowerProfile::OclPowerProfile(xrt::device* xrt_device, 
                                std::shared_ptr<XoclPlugin> xocl_plugin,
                                std::string unique_name) 
                                : status(PowerProfileStatus::IDLE) {
    power_profile_en = xrt::config::get_power_profile();
    target_device = xrt_device;
    target_xocl_plugin = xocl_plugin;
    target_unique_name = unique_name;
    output_file_name = "power_profile_" + target_unique_name + ".csv";
    if (power_profile_en) {
        start_polling();
    }
}

OclPowerProfile::~OclPowerProfile() {
    if (power_profile_en) {
        stop_polling();
        polling_thread.join();
        power_profiling_output.open(output_file_name, std::ios::out);
        write_header();
        write_trace();
        power_profiling_output.close();
    }
}

void OclPowerProfile::poll_power() {
    std::string subdev = "xmc";
    // TODO: prepare all the sysfs paths
    std::vector<std::string> entries = {
        "xmc_12v_aux_curr",
        "xmc_12v_aux_vol",
        "xmc_12v_pex_curr",
        "xmc_12v_pex_vol",
        "xmc_vccint_curr",
        "xmc_vccint_vol",
        "xmc_3v3_pex_curr",
        "xmc_3v3_pex_vol"
    };

    std::vector<std::string> paths;
    for(auto& e : entries) {
        paths.push_back (target_device->getSysfsPath(subdev, e).get());
    }

    while (should_continue()) {
        double timestamp = target_xocl_plugin->getTraceTime();
        power_trace.push_back(std::make_pair(timestamp, std::vector<int>()));

        for (auto& p: paths) {
            std::ifstream fs(p);
            std::string data;
            std::getline(fs, data);
            int dp =  data.empty() ? 0 : std::stoi(data);
            power_trace.back().second.push_back(dp);
            fs.close();
        }

        // TODO: step 3 pause the thread for certain time
        std::this_thread::sleep_for (std::chrono::milliseconds(20));
    }
}

bool OclPowerProfile::should_continue() {
    std::lock_guard<std::mutex> lock(status_lock);
    return status == PowerProfileStatus::POLLING;
}

void OclPowerProfile::start_polling() {
    std::lock_guard<std::mutex> lock(status_lock);
    status = PowerProfileStatus::POLLING;
    polling_thread = std::thread(&OclPowerProfile::poll_power, this);
}

void OclPowerProfile::stop_polling() {
    std::lock_guard<std::mutex> lock(status_lock);
    status = PowerProfileStatus::STOPPING;
}

void OclPowerProfile::write_header() {
    power_profiling_output << "Target device: "
                        << target_unique_name << std::endl;
    power_profiling_output << "timestamp,"
                        << "12v_aux_curr" << ","
                        << "12v_aux_vol"  << ","
                        << "12v_pex_curr" << ","
                        << "12v_pex_vol"  << ","
                        << "vccint_curr"  << ","
                        << "vccint_vol"   <<","
                        << "3v3_pex_curr" << ","
                        << "3v3_pex_vol"
                        << std::endl;
}

void OclPowerProfile::write_trace() {
    for (auto& power_stat : power_trace) {
        power_profiling_output << power_stat.first << ",";
        for (auto data : power_stat.second) {
            power_profiling_output << data << ",";
        }
        power_profiling_output << std::endl;
    }
}

}