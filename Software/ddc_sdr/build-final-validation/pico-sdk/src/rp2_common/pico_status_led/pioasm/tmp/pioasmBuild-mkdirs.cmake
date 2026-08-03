# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/pico-ice-sdk/lib/pico-sdk/tools/pioasm"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pioasm"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pioasm-install"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/tmp"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/src"
  "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/frohro/Classes/Digital_Design/Pico-Dev-iCE/Software/ddc_sdr/build-final-validation/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
