/*
// Copyright (c) 2018-2021 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
*/

#include <fstream>
#include <iomanip>

#include "common.h"
#include "intercept.h"

#define CL_PROFILING_COMMAND_PERFCOUNTERS_INTEL 0x407F

///////////////////////////////////////////////////////////////////////////////
//
static bool convertPropertiesToOCL1_2(
    const cl_queue_properties* properties,
    cl_command_queue_properties& ocl1_2_properties )
{
    if( properties )
    {
        // Convert properties from array of pairs (OCL2.0) to bitfield (OCL1.2)
        for( int i = 0; properties[ i ] != 0; i += 2 )
        {
            switch( properties[ i ] )
            {
            case CL_QUEUE_PROPERTIES:
                switch( properties[ i + 1 ] )
                {
                case 0: // no special queue properties
                case CL_QUEUE_PROFILING_ENABLE:
                case CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE:
                case CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE:
                    ocl1_2_properties |= properties[ i + 1 ];
                    break;
                default:
                    return false;
                }
                break;
            case CL_QUEUE_PRIORITY_KHR:
            case CL_QUEUE_THROTTLE_KHR:
                // Skip / ignore these properties.
                break;
            default:
                return false;
            }
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
//
void CLIntercept::initCustomPerfCounters()
{
    const std::string& metricSetSymbolName = config().DevicePerfCounterCustom;
    const std::string& metricsFileName = config().DevicePerfCounterFile;
    const bool includeMaxValues = config().DevicePerfCounterReportMax;

    if( m_pMDHelper == NULL )
    {
        if( config().DevicePerfCounterEventBasedSampling )
        {
            m_pMDHelper = MetricsDiscovery::MDHelper::CreateEBS(
                config().DevicePerfCounterLibName,
                metricSetSymbolName,
                metricsFileName,
                includeMaxValues );
        }
        else if( config().DevicePerfCounterTimeBasedSampling )
        {
            m_pMDHelper = MetricsDiscovery::MDHelper::CreateTBS(
                config().DevicePerfCounterLibName,
                metricSetSymbolName,
                metricsFileName,
                includeMaxValues );
        }
        else
        {
            CLI_ASSERT( 0 );
        }
        if( m_pMDHelper )
        {
            log( "Metric Discovery initialized.\n" );
        }
        else
        {
            log( "Metric Discovery failed to initialize.\n" );
        }
    }

    if( m_pMDHelper )
    {
        // Open the metric stream for time based sampling, if needed.
        if( config().DevicePerfCounterTimeBasedSampling )
        {
            uint32_t    timerMS = 10;
            m_pMDHelper->OpenStream(
                timerMS * 1000000,  // timer period, in nanoseconds
                0,                  // buffer size in bytes -> device maximum
                0 );                // pid -> sample all processes
        }

        // Get the dump directory name and create the dump file for
        // metrics, if we haven't created it already.
        if ( !m_MetricDump.is_open() )
        {
            std::string fileName = "";
            OS().GetDumpDirectoryName( sc_DumpDirectoryName, fileName );
            fileName += '/';
            fileName += sc_DumpPerfCountersFileNamePrefix;
            fileName += "_";
            fileName += metricSetSymbolName;
            fileName += ".csv";

            OS().MakeDumpDirectories( fileName );

            m_MetricDump.open( fileName.c_str(), std::ios::out );

            m_pMDHelper->PrintMetricNames( m_MetricDump );
            m_pMDHelper->PrintMetricUnits( m_MetricDump );
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//
cl_command_queue CLIntercept::createMDAPICommandQueue(
    cl_context context,
    cl_device_id device,
    cl_command_queue_properties properties,
    cl_int* errcode_ret )
{
    cl_command_queue    retVal = NULL;

    cl_platform_id  platform = getPlatform(device);

    if( dispatchX(platform).clCreatePerfCountersCommandQueueINTEL == NULL )
    {
        getExtensionFunctionAddress(
            platform,
            "clCreatePerfCountersCommandQueueINTEL" );
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    auto dispatchX = this->dispatchX(platform);
    if( dispatchX.clCreatePerfCountersCommandQueueINTEL == NULL )
    {
        log( "Couldn't get pointer to clCreatePerfCountersCommandQueueINTEL!\n" );
    }
    else if( m_pMDHelper == NULL )
    {
        log( "Metrics discovery is not initialized!\n" );
    }
    else
    {
        if( m_pMDHelper->ActivateMetricSet() )
        {
            cl_int  errorCode = CL_SUCCESS;
            cl_uint configuration = m_pMDHelper->GetMetricsConfiguration();
            logf( "Calling clCreatePerfCountersCommandQueueINTEL with configuration %u....\n",
                configuration);

            retVal = dispatchX.clCreatePerfCountersCommandQueueINTEL(
                context,
                device,
                properties,
                configuration,
                &errorCode );
            if( retVal == NULL )
            {
                logf( "clCreatePerfCountersCommandQueueINTEL returned %s (%d)!\n",
                    enumName().name( errorCode ).c_str(),
                    errorCode );
            }
            else
            {
                log( "clCreatePerfCountersCommandQueueINTEL succeeded.\n" );
            }
            if( errcode_ret )
            {
                errcode_ret[0] = errorCode;
            }
        }
        else
        {
            log( "Metric Discovery: Couldn't activate metric set!\n" );
        }
    }

    return retVal;
}

///////////////////////////////////////////////////////////////////////////////
//
cl_command_queue CLIntercept::createMDAPICommandQueue(
    cl_context context,
    cl_device_id device,
    const cl_queue_properties* properties,
    cl_int* errcode_ret )
{
    cl_command_queue    retVal = NULL;

    // This is a temporary workaround until we have a
    // clCreatePerfCountersCommandQueueWithPropertiesINTEL API.
    // It converts the OpenCL 2.0 command queue properties to
    // OpenCL 1.2 command queue properties, unless an unsupported
    // command queue property is specified.  If an unsupported
    // property is specified then we cannot create an MDAPI command
    // queue.

    cl_command_queue_properties ocl1_2_properties = 0;
    if( convertPropertiesToOCL1_2( properties, ocl1_2_properties ) )
    {
        retVal = createMDAPICommandQueue(
            context,
            device,
            ocl1_2_properties,
            errcode_ret );
    }

    return retVal;
}

///////////////////////////////////////////////////////////////////////////////
//
void CLIntercept::getMDAPICountersFromStream( void )
{
    // We should only get here when time based sampling is enabled.
    CLI_ASSERT( config().DevicePerfCounterTimeBasedSampling );

    if( m_pMDHelper )
    {
        std::vector<MetricsDiscovery::TTypedValue_1_0> results;
        std::vector<MetricsDiscovery::TTypedValue_1_0> maxValues;
        std::vector<MetricsDiscovery::TTypedValue_1_0> ioInfoValues;

        while( true )
        {
            bool report = m_pMDHelper->SaveReportsFromStream();
            if( report )
            {
                uint32_t numResults = m_pMDHelper->GetMetricsFromSavedReports(
                    results,
                    maxValues );
                m_pMDHelper->GetIOMeasurementInformation(
                    ioInfoValues );

                m_pMDHelper->PrintMetricValues(
                    m_MetricDump,
                    "TBS",
                    numResults,
                    results,
                    maxValues,
                    ioInfoValues );

                m_pMDHelper->ResetSavedReports();
            }
            else
            {
                break;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//
void CLIntercept::getMDAPICountersFromEvent(
    const std::string& name,
    const cl_event event )
{
    // We should only get here when event based sampling is enabled.
    CLI_ASSERT( config().DevicePerfCounterEventBasedSampling );

    if( m_pMDHelper )
    {
        const size_t reportSize = m_pMDHelper->GetQueryReportSize();

        char*   pReport = new char[ reportSize ];
        if( pReport )
        {
            size_t  outputSize = 0;
            cl_int  errorCode = dispatch().clGetEventProfilingInfo(
                event,
                CL_PROFILING_COMMAND_PERFCOUNTERS_INTEL,
                reportSize,
                pReport,
                &outputSize );

            if( errorCode == CL_SUCCESS )
            {
                // Check: The size of the queried report should be the expected size.
                CLI_ASSERT( outputSize == reportSize );

                std::vector<MetricsDiscovery::TTypedValue_1_0> results;
                std::vector<MetricsDiscovery::TTypedValue_1_0> maxValues;
                std::vector<MetricsDiscovery::TTypedValue_1_0> ioInfoValues; // unused

                uint32_t numResults = m_pMDHelper->GetMetricsFromReports(
                    1,
                    pReport,
                    results,
                    maxValues );

                if( numResults )
                {
                    m_pMDHelper->PrintMetricValues(
                        m_MetricDump,
                        name,
                        numResults,
                        results,
                        maxValues,
                        ioInfoValues );
                    m_pMDHelper->AggregateMetrics(
                        m_MetricAggregations,
                        name,
                        results );
                }
            }
            else
            {
                logf("Couldn't get MDAPI data!  clGetEventProfilingInfo returned '%s' (%08X)!\n",
                    enumName().name(errorCode).c_str(),
                    errorCode );
            }

            delete [] pReport;
            pReport = NULL;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//
void CLIntercept::reportMDAPICounters( std::ostream& os )
{
    if( config().DevicePerfCounterTiming &&
        config().DevicePerfCounterEventBasedSampling &&
        !m_MetricAggregations.empty() )
    {
        std::string header;
        std::vector<uint32_t> headerWidths;
        for( auto& metric : m_MetricAggregations.begin()->second )
        {
            const std::string& metricName = metric.first;

            header += metricName + ", ";
            headerWidths.push_back((uint32_t)metricName.length());
        }

        os << std::endl << "Device Performance Counter Timing: (Average metric per enqueue)" << std::endl;
        os << "                                FunctionName,  Calls, " << header;

        for( auto& metricsForKernel : m_MetricAggregations )
        {
            const std::string& kernelName = metricsForKernel.first;
            const MetricsDiscovery::CMetricAggregationsForKernel& kernelMetrics = metricsForKernel.second;

            uint64_t count = kernelMetrics.begin()->second.Count;
            os << std::endl << std::right << std::setw( 44 ) << kernelName << ", ";
            os << std::right << std::setw( 6 ) << count << ", ";

            int numMetric = 0;
            for( auto& metric : kernelMetrics )
            {
                const MetricsDiscovery::SMetricAggregationData& aggregationData = metric.second;
                os << std::right << std::setw( headerWidths[ numMetric++ ] );
                os << aggregationData.Sum / aggregationData.Count << ", ";
            }
        }

        os << std::endl;
    }
}
