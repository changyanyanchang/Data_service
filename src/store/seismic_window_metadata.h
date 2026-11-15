#pragma once
#include "paleo_data/seismic_window.h"
namespace paleo::data::detail {
class ProjectStore;
bool seismicWindowOptions(const SeismicWindowReadOptions&, std::string&);
bool seismicWindowStopped(const SeismicWindowReadOptions&, std::string&);
bool seismicWindowDescriptor(const ProjectStore&, std::int64_t, SeismicWindowDescriptor&,
    const SeismicWindowReadOptions&, SeismicWindowReadStats&, std::string&);
bool decodeAuthenticatedSeismicWindow(const SeismicWindowQuery&, const SeismicAuthenticatedSource&,
    const SeismicWindowDescriptor&, SeismicWindow&, const SeismicWindowReadOptions&,
    SeismicWindowReadStats&, std::string&);
}
