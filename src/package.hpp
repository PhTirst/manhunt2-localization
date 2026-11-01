#pragma once
#include "project.hpp"
namespace mh2
{
    void CaptureOriginals(const ProjectConfig &project);
    void SealOriginals(const ProjectConfig &project);
    bool BuildProject(const ProjectConfig &project, bool force = false);
    void DeployProject(const ProjectConfig &project);
    void RestoreProject(const ProjectConfig &project);
}
