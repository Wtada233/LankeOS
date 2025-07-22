#ifndef PACKAGE_MANAGER_HPP
#define PACKAGE_MANAGER_HPP

#include <string>

void install_package(const std::string& pkg_name, const std::string& version);
void remove_package(const std::string& pkg_name, bool force = false);
void autoremove();
void upgrade_packages();
void show_man_page(const std::string& pkg_name);

#endif // PACKAGE_MANAGER_HPP
