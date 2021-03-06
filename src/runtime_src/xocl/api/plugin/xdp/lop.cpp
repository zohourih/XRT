/**
 * Copyright (C) 2016-2020 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#ifndef _WIN32

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "core/common/dlfcn.h"
#include "lop.h"

#ifdef _WIN32
#pragma warning (disable : 4996)
/* Disable warning for use of getenv */
#endif

namespace bfs = boost::filesystem;

// Helper functions for loading the plugin library
namespace {

  static const char* emptyOrValue(const char* cstr)
  {
    return cstr ? cstr : "" ;
  }

  static boost::filesystem::path& dllExt()
  {
#ifdef _WIN32
    static boost::filesystem::path sDllExt(".dll");
#else
    static boost::filesystem::path sDllExt(".so");
#endif
    return sDllExt;
  }

  static bool isDLL(const bfs::path& path)
  {
    return (bfs::exists(path) && bfs::is_regular_file(path) && path.extension() == dllExt()) ;
  }

  static boost::filesystem::path
  dllpath(const boost::filesystem::path& root, const std::string& libnm)
  {
#ifdef _WIN32
    return root / "bin" / (libnm + ".dll");
#else
    return root / "lib" / ("lib" + libnm + ".so");
#endif
  }
} // end anonymous namespace

namespace xdplop {

  // The loading of the function should only happen once.  Since it 
  //  could theoretically be called from two user threads at once, we
  //  use an internal struct constructor that is thread safe to ensure
  //  it only happens once
  void load_xdp_lop()
  {
    struct xdp_lop_once_loader
    {
      xdp_lop_once_loader()
      {
	bfs::path xrt(emptyOrValue(getenv("XILINX_XRT")));
	if (xrt.empty()) 
	  throw std::runtime_error("XILINX_XRT not set");
	
	bfs::path xrtlib(xrt / "lib");
	if (!bfs::is_directory(xrtlib))
	  throw std::runtime_error("No such directory '"+xrtlib.string()+"'");

	auto libpath = dllpath(xrt, "xdp_lop_plugin");
	if (!isDLL(libpath)) 
	  throw std::runtime_error("Library "+libpath.string()+" not found!");
	
	auto handle = 
	  xrt_core::dlopen(libpath.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
	if (!handle)
	  throw std::runtime_error("Failed to open XDP library '" + libpath.string() + "'\n" + xrt_core::dlerror());
	
	register_lop_functions(handle) ;
      }
    };

    // Thread safe per C++-11
    static xdp_lop_once_loader xdp_lop_loaded ;
  }

  // All of the function pointers that will be dynamically linked from
  //  the XDP Plugin side
  std::function<void (const char*, long long int, unsigned int)> function_start_cb;
  std::function<void (const char*, long long int, unsigned int)> function_end_cb;
  std::function<void (unsigned int, bool)> read_cb ;
  std::function<void (unsigned int, bool)> write_cb ;
  std::function<void (unsigned int, bool)> enqueue_cb ;

  void register_lop_functions(void* handle)
  {
    typedef void (*ftype)(const char*, long long int, unsigned int) ;
    function_start_cb = (ftype)(xrt_core::dlsym(handle, "lop_function_start")) ;
    if (xrt_core::dlerror() != NULL) function_start_cb = nullptr ;    

    function_end_cb = (ftype)(xrt_core::dlsym(handle, "lop_function_end"));
    if (xrt_core::dlerror() != NULL) function_end_cb = nullptr ;

    typedef void (*btype)(unsigned int, bool) ;

    read_cb = (btype)(xrt_core::dlsym(handle, "lop_read")) ;
    if (xrt_core::dlerror() != NULL) read_cb = nullptr ;
    
    write_cb = (btype)(xrt_core::dlsym(handle, "lop_write")) ;
    if (xrt_core::dlerror() != NULL) write_cb = nullptr ;

    enqueue_cb = (btype)(xrt_core::dlsym(handle, "lop_kernel_enqueue")) ;
    if (xrt_core::dlerror() != NULL) enqueue_cb = nullptr ;
  }

  std::atomic<unsigned int> LOPFunctionCallLogger::m_funcid_global(0) ;

  LOPFunctionCallLogger::LOPFunctionCallLogger(const char* function) :
    LOPFunctionCallLogger(function, 0)
  {    
  }

  LOPFunctionCallLogger::LOPFunctionCallLogger(const char* function, 
					       long long int address) :
    m_name(function), m_address(address)
  {
    // Load the LOP plugin if not already loaded
    static bool s_load_lop = false ;
    if (!s_load_lop)
    {
      s_load_lop = true ;
      if (xrt_core::config::get_lop_profile()) 
	load_xdp_lop() ;
    }

    // Log the stats for this function
    m_funcid = m_funcid_global++ ;
    if (function_start_cb)
      function_start_cb(m_name, m_address, m_funcid) ;
  }

  LOPFunctionCallLogger::~LOPFunctionCallLogger()
  {
    if (function_end_cb)
      function_end_cb(m_name, m_address, m_funcid) ;
  }

} // end namespace xdplop

namespace xocl {
  namespace lop {

    // Create lambda functions that will be attached and triggered
    //  by events when their status changes
    std::function<void (xocl::event*, cl_int)> 
    action_read()
    {
      return [](xocl::event* e, cl_int status) 
	{
	  if (!xdplop::read_cb) return ;

	  // Only keep track of the start and stop
	  if (status == CL_RUNNING)
	    xdplop::read_cb(e->get_uid(), true) ;
	  else if (status == CL_COMPLETE) 
	    xdplop::read_cb(e->get_uid(), false) ;
	} ;
    }

    std::function<void (xocl::event*, cl_int)> 
    action_write()
    {
      return [](xocl::event* e, cl_int status)
	{
	  if (!xdplop::write_cb) return ;

	  // Only keep track of the start and stop
	  if (status == CL_RUNNING)
	    xdplop::write_cb(e->get_uid(), true) ;
	  else if (status == CL_COMPLETE) 
	    xdplop::write_cb(e->get_uid(), false) ;
	} ;
    }

    std::function<void (xocl::event*, cl_int)> 
    action_migrate(cl_mem_migration_flags flags) 
    {
      if (flags & CL_MIGRATE_MEM_OBJECT_HOST)
      {
	return [](xocl::event* e, cl_int status)
	  {
	    if (!xdplop::read_cb) return ;

	    if (status == CL_RUNNING)
	      xdplop::read_cb(e->get_uid(), true) ;
	    else if (status == CL_COMPLETE)
	      xdplop::read_cb(e->get_uid(), false) ;
	  } ;
      }
      else
      {
	return [](xocl::event* e, cl_int status)
	  {
	    if (!xdplop::write_cb) return ;

	    if (status == CL_RUNNING)
	      xdplop::write_cb(e->get_uid(), true) ;
	    else if (status == CL_COMPLETE)
	      xdplop::write_cb(e->get_uid(), false) ;
	  } ;
      }
    }

    std::function<void (xocl::event*, cl_int)> 
    action_ndrange()
    {
      return [](xocl::event* e, cl_int status)
	{
	  if (!xdplop::enqueue_cb) return ;

	  if (status == CL_RUNNING || status == CL_SUBMITTED)
	    xdplop::enqueue_cb(e->get_uid(), true) ;
	  else if (status == CL_COMPLETE)
	    xdplop::enqueue_cb(e->get_uid(), false) ;
	} ;
    }

  } // end namespace lop
} // end namespace xocl

#else 
// LOP is initially only supported on Linux

#endif
