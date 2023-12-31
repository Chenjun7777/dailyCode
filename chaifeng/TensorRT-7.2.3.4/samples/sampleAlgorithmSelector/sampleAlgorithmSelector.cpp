/*
 * Copyright 1993-2021 NVIDIA Corporation.  All rights reserved.
 *
 * NOTICE TO LICENSEE:
 *
 * This source code and/or documentation ("Licensed Deliverables") are
 * subject to NVIDIA intellectual property rights under U.S. and
 * international Copyright laws.
 *
 * These Licensed Deliverables contained herein is PROPRIETARY and
 * CONFIDENTIAL to NVIDIA and is being provided under the terms and
 * conditions of a form of NVIDIA software license agreement by and
 * between NVIDIA and Licensee ("License Agreement") or electronically
 * accepted by Licensee.  Notwithstanding any terms or conditions to
 * the contrary in the License Agreement, reproduction or disclosure
 * of the Licensed Deliverables to any third party without the express
 * written consent of NVIDIA is prohibited.
 *
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, NVIDIA MAKES NO REPRESENTATION ABOUT THE
 * SUITABILITY OF THESE LICENSED DELIVERABLES FOR ANY PURPOSE.  IT IS
 * PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.
 * NVIDIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THESE LICENSED
 * DELIVERABLES, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY,
 * NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY
 * SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, OR ANY
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THESE LICENSED DELIVERABLES.
 *
 * U.S. Government End Users.  These Licensed Deliverables are a
 * "commercial item" as that term is defined at 48 C.F.R. 2.101 (OCT
 * 1995), consisting of "commercial computer software" and "commercial
 * computer software documentation" as such terms are used in 48
 * C.F.R. 12.212 (SEPT 1995) and is provided to the U.S. Government
 * only as a commercial end item.  Consistent with 48 C.F.R.12.212 and
 * 48 C.F.R. 227.7202-1 through 227.7202-4 (JUNE 1995), all
 * U.S. Government End Users acquire the Licensed Deliverables with
 * only those rights set forth herein.
 *
 * Any use of the Licensed Deliverables in individual and commercial
 * software must include, in the user documentation and internal
 * comments to the code, the above Disclaimer and U.S. Government End
 * Users Notice.
 */

//! \file sampleAlgorithmSelector.cpp
//! \brief This file contains the implementation of Algorithm Selector sample.
//!
//! It demonstrates the usage of IAlgorithmSelector to cache the algorithms used in a network.
//! It also shows the usage of IAlgorithmSelector::selectAlgorithms to define heuristics for selection of algorithms.
//! It builds a TensorRT engine by importing a trained MNIST Caffe model and runs inference on an input image of a
//! digit.
//! It can be run with the following command line:
//! Command: ./sample_algorithm_selector [-h or --help] [-d=/path/to/data/dir or --datadir=/path/to/data/dir]

#include "argsParser.h"
#include "buffers.h"
#include "common.h"
#include "logger.h"

#include "NvCaffeParser.h"
#include "NvInfer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cuda_runtime_api.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

const std::string gSampleName = "TensorRT.sample_algorithm_selector";
const std::string gCacheFileName = "AlgorithmCache.txt";
//!
//! \brief Writes the default algorithm choices made by TensorRT into a file.
//!
class AlgorithmCacheWriter : public IAlgorithmSelector
{
public:
    //!
    //! \brief Return value in [0, nbChoices] for a valid algorithm.
    //!
    //! \details Lets TRT use its default tactic selection method.
    //! Writes all the possible choices to the selection buffer and returns the length of it.
    //! If BuilderFlag::kSTRICT_TYPES is not set, just returning 0 forces default tactic selection.
    //!
    int32_t selectAlgorithms(const nvinfer1::IAlgorithmContext& context, const nvinfer1::IAlgorithm* const* choices,
        int32_t nbChoices, int32_t* selection) override
    {
        // TensorRT always provides more than zero number of algorithms in selectAlgorithms.
        assert(nbChoices > 0);

        std::iota(selection, selection + nbChoices, 0);
        return nbChoices;
    }

    //!
    //! \brief called by TensorRT to report choices it made.
    //!
    //! \details Writes the TensorRT algorithm choices into a file.
    //!
    void reportAlgorithms(const nvinfer1::IAlgorithmContext* const* algoContexts,
        const nvinfer1::IAlgorithm* const* algoChoices, int32_t nbAlgorithms) noexcept override
    {
        std::ofstream algorithmFile(mCacheFileName);
        if (!algorithmFile.good())
        {
            sample::gLogError << "Cannot open algorithm cache file: " << mCacheFileName << " to write." << std::endl;
            abort();
        }

        for (int32_t i = 0; i < nbAlgorithms; i++)
        {
            algorithmFile << algoContexts[i]->getName() << "\n";
            algorithmFile << algoChoices[i]->getAlgorithmVariant().getImplementation() << "\n";
            algorithmFile << algoChoices[i]->getAlgorithmVariant().getTactic() << "\n";

            // Write number of inputs and outputs.
            const int32_t nbInputs = algoContexts[i]->getNbInputs();
            algorithmFile << nbInputs << "\n";
            const int32_t nbOutputs = algoContexts[i]->getNbOutputs();
            algorithmFile << nbOutputs << "\n";

            // Write input and output formats.
            for (int32_t j = 0; j < nbInputs + nbOutputs; j++)
            {
                algorithmFile << static_cast<int32_t>(algoChoices[i]->getAlgorithmIOInfo(j).getTensorFormat()) << "\n";
                algorithmFile << static_cast<int32_t>(algoChoices[i]->getAlgorithmIOInfo(j).getDataType()) << "\n";
            }
        }
        algorithmFile.close();
    }

    AlgorithmCacheWriter(const std::string& cacheFileName)
        : mCacheFileName(cacheFileName)
    {
    }

private:
    std::string mCacheFileName;
};

//!
//! \brief Replicates the algorithm selection using a cache file.
//!
class AlgorithmCacheReader : public IAlgorithmSelector
{
public:
    //!
    //! \brief Return value in [0, nbChoices] for a valid algorithm.
    //!
    //! \details Use the map created from cache to select algorithms.
    //!
    int32_t selectAlgorithms(const nvinfer1::IAlgorithmContext& algoContext,
        const nvinfer1::IAlgorithm* const* algoChoices, int32_t nbChoices, int32_t* selection) override
    {
        // TensorRT always provides more than zero number of algorithms in selectAlgorithms.
        assert(nbChoices > 0);

        const std::string layerName(algoContext.getName());
        auto it = choiceMap.find(layerName);

        // The layerName can be used as a unique identifier for a layer.
        // Since the network and config has not been changed (between the cache and cache read),
        // This map must contain layerName.
        assert(it != choiceMap.end());
        auto& algoItem = it->second;

        assert(algoItem.nbInputs == algoContext.getNbInputs());
        assert(algoItem.nbOutputs == algoContext.getNbOutputs());

        int32_t nbSelections = 0;
        for (auto i = 0; i < nbChoices; i++)
        {
            // The combination of implementation, tactic and input/output formats is unique to an algorithm,
            // and can be used to reproduce the same algorithm. Since the network and config has not been changed
            // (between the cache and cache read), there must be exactly one algorithm match for each layerName.
            if (areSame(algoItem, *algoChoices[i]))
            {
                selection[nbSelections++] = i;
            }
        }

        //! There must be only one algorithm selected.
        assert(nbSelections == 1);
        return nbSelections;
    }

    //!
    //! \brief Called by TensorRT to report choices it made.
    //!
    //! \details Verifies that the algorithm used by TensorRT conform to the cache.
    //!
    void reportAlgorithms(const nvinfer1::IAlgorithmContext* const* algoContexts,
        const nvinfer1::IAlgorithm* const* algoChoices, int32_t nbAlgorithms) override
    {
        for (auto i = 0; i < nbAlgorithms; i++)
        {
            const std::string layerName(algoContexts[i]->getName());
            assert(choiceMap.find(layerName) != choiceMap.end());
            const auto& algoItem = choiceMap[layerName];
            assert(algoItem.nbInputs == algoContexts[i]->getNbInputs());
            assert(algoItem.nbOutputs == algoContexts[i]->getNbOutputs());
            assert(algoChoices[i]->getAlgorithmVariant().getImplementation() == algoItem.implementation);
            assert(algoChoices[i]->getAlgorithmVariant().getTactic() == algoItem.tactic);
            auto nbFormats = algoItem.nbInputs + algoItem.nbOutputs;
            for (auto j = 0; j < nbFormats; j++)
            {
                assert(algoItem.formats[j].first
                    == static_cast<int32_t>(algoChoices[i]->getAlgorithmIOInfo(j).getTensorFormat()));
                assert(algoItem.formats[j].second
                    == static_cast<int32_t>(algoChoices[i]->getAlgorithmIOInfo(j).getDataType()));
            }
        }
    }

    AlgorithmCacheReader(const std::string& cacheFileName)
    {
        //! Use the cache file to create a map of algorithm choices.
        std::ifstream algorithmFile(cacheFileName);
        if (!algorithmFile.good())
        {
            sample::gLogError << "Cannot open algorithm cache file: " << cacheFileName << " to read." << std::endl;
            abort();
        }

        std::string line;
        while (getline(algorithmFile, line))
        {
            std::string layerName;
            layerName = line;

            AlgorithmCacheItem algoItem;
            getline(algorithmFile, line);
            algoItem.implementation = std::stoll(line);

            getline(algorithmFile, line);
            algoItem.tactic = std::stoll(line);

            getline(algorithmFile, line);
            algoItem.nbInputs = std::stoi(line);

            getline(algorithmFile, line);
            algoItem.nbOutputs = std::stoi(line);

            const int32_t nbFormats = algoItem.nbInputs + algoItem.nbOutputs;
            algoItem.formats.resize(nbFormats);
            for (int32_t i = 0; i < nbFormats; i++)
            {
                getline(algorithmFile, line);
                algoItem.formats[i].first = std::stoi(line);
                getline(algorithmFile, line);
                algoItem.formats[i].second = std::stoi(line);
            }
            choiceMap[layerName] = std::move(algoItem);
        }
        algorithmFile.close();
    }

private:
    struct AlgorithmCacheItem
    {
        int64_t implementation;
        int64_t tactic;
        int32_t nbInputs;
        int32_t nbOutputs;
        std::vector<std::pair<int32_t, int32_t>> formats;
    };
    std::unordered_map<std::string, AlgorithmCacheItem> choiceMap;

    //! The combination of implementation, tactic and input/output formats is unique to an algorithm,
    //! and can be used to check if two algorithms are same.
    static bool areSame(const AlgorithmCacheItem& algoCacheItem, const IAlgorithm& algoChoice)
    {
        if (algoChoice.getAlgorithmVariant().getImplementation() != algoCacheItem.implementation
            || algoChoice.getAlgorithmVariant().getTactic() != algoCacheItem.tactic)
        {
            return false;
        }

        // Loop over all the AlgorithmIOInfos to see if all of them match to the formats in algo item.
        const auto nbFormats = algoCacheItem.nbInputs + algoCacheItem.nbOutputs;
        for (auto j = 0; j < nbFormats; j++)
        {
            if (algoCacheItem.formats[j].first
                    != static_cast<int32_t>(algoChoice.getAlgorithmIOInfo(j).getTensorFormat())
                || algoCacheItem.formats[j].second
                    != static_cast<int32_t>(algoChoice.getAlgorithmIOInfo(j).getDataType()))
            {
                return false;
            }
        }

        return true;
    }
};

//!
//! \brief Selects Algorithms with minimum workspace requirements.
//!
class MinimumWorkspaceAlgorithmSelector : public IAlgorithmSelector
{
public:
    //!
    //! \brief Return value in [0, nbChoices] for a valid algorithm.
    //!
    //! \details Use the map created from cache to select algorithms.
    //!
    int32_t selectAlgorithms(const nvinfer1::IAlgorithmContext& algoContext,
        const nvinfer1::IAlgorithm* const* algoChoices, int32_t nbChoices, int32_t* selection) override
    {
        // TensorRT always provides more than zero number of algorithms in selectAlgorithms.
        assert(nbChoices > 0);

        auto it = std::min_element(
            algoChoices, algoChoices + nbChoices, [](const nvinfer1::IAlgorithm* x, const nvinfer1::IAlgorithm* y) {
                return x->getWorkspaceSize() < y->getWorkspaceSize();
            });
        selection[0] = static_cast<int32_t>(it - algoChoices);
        return 1;
    }

    //!
    //! \brief Called by TensorRT to report choices it made.
    //!
    void reportAlgorithms(const nvinfer1::IAlgorithmContext* const* algoContexts,
        const nvinfer1::IAlgorithm* const* algoChoices, int32_t nbAlgorithms) override
    {
        // do nothing
    }
};

//!
//! \brief  The SampleAlgorithmSelector class implements the SampleAlgorithmSelector sample.
//!
//! \details It creates the network using a trained Caffe MNIST classification model.
//!
class SampleAlgorithmSelector
{
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

public:
    SampleAlgorithmSelector(const samplesCommon::CaffeSampleParams& params)
        : mParams(params)
    {
    }

    //!
    //! \brief Builds the network engine.
    //!
    bool build(IAlgorithmSelector* selector);

    //!
    //! \brief Runs the TensorRT inference engine for this sample.
    //!
    bool infer();

    //!
    //! \brief Used to clean up any state created in the sample class.
    //!
    bool teardown();

private:
    //!
    //! \brief uses a Caffe parser to create the MNIST Network and marks the output layers.
    //!
    bool constructNetwork(
        SampleUniquePtr<nvcaffeparser1::ICaffeParser>& parser, SampleUniquePtr<nvinfer1::INetworkDefinition>& network);

    //!
    //! \brief Reads the input and mean data, preprocesses, and stores the result in a managed buffer.
    //!
    bool processInput(
        const samplesCommon::BufferManager& buffers, const std::string& inputTensorName, int inputFileIdx) const;

    //!
    //! \brief Verifies that the output is correct and prints it.
    //!
    bool verifyOutput(
        const samplesCommon::BufferManager& buffers, const std::string& outputTensorName, int groundTruthDigit) const;

    std::shared_ptr<nvinfer1::ICudaEngine> mEngine{nullptr}; //!< The TensorRT engine used to run the network.

    samplesCommon::CaffeSampleParams mParams; //!< The parameters for the sample.

    nvinfer1::Dims mInputDims; //!< The dimensions of the input to the network.

    SampleUniquePtr<nvcaffeparser1::IBinaryProtoBlob>
        mMeanBlob; //! the mean blob, which we need to keep around until build is done.
};

//!
//! \brief Creates the network, configures the builder and creates the network engine.
//!
//! \details This function creates the MNIST network by parsing the caffe model and builds
//!          the engine that will be used to run MNIST (mEngine).
//!
//! \return Returns true if the engine was created successfully and false otherwise.
//!
bool SampleAlgorithmSelector::build(IAlgorithmSelector* selector)
{
    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(sample::gLogger.getTRTLogger()));
    if (!builder)
    {
        return false;
    }

    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetwork());
    if (!network)
    {
        return false;
    }

    auto config = SampleUniquePtr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    if (!config)
    {
        return false;
    }

    auto parser = SampleUniquePtr<nvcaffeparser1::ICaffeParser>(nvcaffeparser1::createCaffeParser());
    if (!parser)
    {
        return false;
    }

    if (!constructNetwork(parser, network))
    {
        return false;
    }

    builder->setMaxBatchSize(mParams.batchSize);
    config->setMaxWorkspaceSize(16_MiB);
    config->setAlgorithmSelector(selector);
    config->setFlag(BuilderFlag::kGPU_FALLBACK);

    if (!mParams.int8)
    {
        // The sample fails for Int8 with kSTRICT_TYPES flag set.
        config->setFlag(BuilderFlag::kSTRICT_TYPES);
    }
    if (mParams.fp16)
    {
        config->setFlag(BuilderFlag::kFP16);
    }
    if (mParams.int8)
    {
        config->setFlag(BuilderFlag::kINT8);
    }

    samplesCommon::enableDLA(builder.get(), config.get(), mParams.dlaCore);
    mEngine = std::shared_ptr<nvinfer1::ICudaEngine>(
        builder->buildEngineWithConfig(*network, *config), samplesCommon::InferDeleter());

    if (!mEngine)
    {
        return false;
    }

    assert(network->getNbInputs() == 1);
    mInputDims = network->getInput(0)->getDimensions();
    assert(mInputDims.nbDims == 3);

    return true;
}

//!
//! \brief Reads the input and mean data, preprocesses, and stores the result in a managed buffer.
//!
bool SampleAlgorithmSelector::processInput(
    const samplesCommon::BufferManager& buffers, const std::string& inputTensorName, int inputFileIdx) const
{
    const int inputH = mInputDims.d[1];
    const int inputW = mInputDims.d[2];

    // Read a random digit file.
    srand(unsigned(time(nullptr)));
    std::vector<uint8_t> fileData(inputH * inputW);
    readPGMFile(locateFile(std::to_string(inputFileIdx) + ".pgm", mParams.dataDirs), fileData.data(), inputH, inputW);

    // Print ASCII representation of digit.
    sample::gLogInfo << "Input:\n";
    for (int i = 0; i < inputH * inputW; i++)
    {
        sample::gLogInfo << (" .:-=+*#%@"[fileData[i] / 26]) << (((i + 1) % inputW) ? "" : "\n");
    }
    sample::gLogInfo << std::endl;

    float* hostInputBuffer = static_cast<float*>(buffers.getHostBuffer(inputTensorName));

    for (int i = 0; i < inputH * inputW; i++)
    {
        hostInputBuffer[i] = float(fileData[i]);
    }

    return true;
}

//!
//! \brief Verifies that the output is correct and prints it.
//!
bool SampleAlgorithmSelector::verifyOutput(
    const samplesCommon::BufferManager& buffers, const std::string& outputTensorName, int groundTruthDigit) const
{
    const float* prob = static_cast<const float*>(buffers.getHostBuffer(outputTensorName));

    // Print histogram of the output distribution.
    sample::gLogInfo << "Output:\n";
    float val{0.0f};
    int idx{0};
    const int kDIGITS = 10;

    for (int i = 0; i < kDIGITS; i++)
    {
        if (val < prob[i])
        {
            val = prob[i];
            idx = i;
        }

        sample::gLogInfo << i << ": " << std::string(int(std::floor(prob[i] * 10 + 0.5f)), '*') << "\n";
    }
    sample::gLogInfo << std::endl;

    return (idx == groundTruthDigit && val > 0.9f);
}

//!
//! \brief Uses a caffe parser to create the MNIST Network and marks the
//!        output layers.
//!
//! \param network Pointer to the network that will be populated with the MNIST network.
//!
//! \param builder Pointer to the engine builder.
//!
bool SampleAlgorithmSelector::constructNetwork(
    SampleUniquePtr<nvcaffeparser1::ICaffeParser>& parser, SampleUniquePtr<nvinfer1::INetworkDefinition>& network)
{
    const nvcaffeparser1::IBlobNameToTensor* blobNameToTensor = parser->parse(
        mParams.prototxtFileName.c_str(), mParams.weightsFileName.c_str(), *network, nvinfer1::DataType::kFLOAT);

    for (auto& s : mParams.outputTensorNames)
    {
        network->markOutput(*blobNameToTensor->find(s.c_str()));
    }

    // add mean subtraction to the beginning of the network.
    nvinfer1::Dims inputDims = network->getInput(0)->getDimensions();
    mMeanBlob
        = SampleUniquePtr<nvcaffeparser1::IBinaryProtoBlob>(parser->parseBinaryProto(mParams.meanFileName.c_str()));
    nvinfer1::Weights meanWeights{nvinfer1::DataType::kFLOAT, mMeanBlob->getData(), inputDims.d[1] * inputDims.d[2]};
    // For this sample, a large range based on the mean data is chosen and applied to the head of the network.
    // After the mean subtraction occurs, the range is expected to be between -127 and 127, so the rest of the network
    // is given a generic range.
    // The preferred method is use scales computed based on a representative data set
    // and apply each one individually based on the tensor. The range here is large enough for the
    // network, but is chosen for example purposes only.
    float maxMean
        = samplesCommon::getMaxValue(static_cast<const float*>(meanWeights.values), samplesCommon::volume(inputDims));

    auto mean = network->addConstant(nvinfer1::Dims3(1, inputDims.d[1], inputDims.d[2]), meanWeights);
    if (!mean->getOutput(0)->setDynamicRange(-maxMean, maxMean))
    {
        return false;
    }
    if (!network->getInput(0)->setDynamicRange(-maxMean, maxMean))
    {
        return false;
    }
    auto meanSub = network->addElementWise(*network->getInput(0), *mean->getOutput(0), ElementWiseOperation::kSUB);
    if (!meanSub->getOutput(0)->setDynamicRange(-maxMean, maxMean))
    {
        return false;
    }
    network->getLayer(0)->setInput(0, *meanSub->getOutput(0));
    samplesCommon::setAllTensorScales(network.get(), 127.0f, 127.0f);

    return true;
}

//!
//! \brief Runs the TensorRT inference engine for this sample.
//!
//! \details This function is the main execution function of the sample. It allocates
//!          the buffer, sets inputs, executes the engine, and verifies the output.
//!
bool SampleAlgorithmSelector::infer()
{
    // Create RAII buffer manager object.
    samplesCommon::BufferManager buffers(mEngine, mParams.batchSize);

    auto context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    if (!context)
    {
        return false;
    }

    // Pick a random digit to try to infer.
    srand(time(NULL));
    const int digit = rand() % 10;

    // Read the input data into the managed buffers.
    // There should be just 1 input tensor.
    assert(mParams.inputTensorNames.size() == 1);
    if (!processInput(buffers, mParams.inputTensorNames[0], digit))
    {
        return false;
    }
    // Create CUDA stream for the execution of this inference.
    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // Asynchronously copy data from host input buffers to device input buffers
    buffers.copyInputToDeviceAsync(stream);

    // Asynchronously enqueue the inference work
    if (!context->enqueue(mParams.batchSize, buffers.getDeviceBindings().data(), stream, nullptr))
    {
        return false;
    }
    // Asynchronously copy data from device output buffers to host output buffers.
    buffers.copyOutputToHostAsync(stream);

    // Wait for the work in the stream to complete.
    cudaStreamSynchronize(stream);

    // Release stream.
    cudaStreamDestroy(stream);

    // Check and print the output of the inference.
    // There should be just one output tensor.
    assert(mParams.outputTensorNames.size() == 1);
    bool outputCorrect = verifyOutput(buffers, mParams.outputTensorNames[0], digit);

    return outputCorrect;
}

//!
//! \brief Used to clean up any state created in the sample class.
//!
bool SampleAlgorithmSelector::teardown()
{
    //! Clean up the libprotobuf files as the parsing is complete.
    //! \note It is not safe to use any other part of the protocol buffers library after
    //! ShutdownProtobufLibrary() has been called.
    nvcaffeparser1::shutdownProtobufLibrary();
    return true;
}

//!
//! \brief Initializes members of the params struct using the command line args
//!
samplesCommon::CaffeSampleParams initializeSampleParams(const samplesCommon::Args& args)
{
    samplesCommon::CaffeSampleParams params;
    if (args.dataDirs.empty()) //!< Use default directories if user hasn't provided directory paths.
    {
        params.dataDirs.push_back("data/mnist/");
        params.dataDirs.push_back("data/samples/mnist/");
    }
    else //!< Use the data directory provided by the user.
    {
        params.dataDirs = args.dataDirs;
    }

    params.prototxtFileName = locateFile("mnist.prototxt", params.dataDirs);
    params.weightsFileName = locateFile("mnist.caffemodel", params.dataDirs);
    params.meanFileName = locateFile("mnist_mean.binaryproto", params.dataDirs);
    params.inputTensorNames.push_back("data");
    params.batchSize = 1;
    params.outputTensorNames.push_back("prob");
    params.dlaCore = args.useDLACore;
    params.int8 = args.runInInt8;
    params.fp16 = args.runInFp16;

    return params;
}

//!
//! \brief Prints the help information for running this sample.
//!
void printHelpInfo()
{
    std::cout << "Usage: ./sample_algorithm_selector [-h or --help] [-d or --datadir=<path to data directory>] "
                 "[--useDLACore=<int>]\n";
    std::cout << "--help          Display help information\n";
    std::cout << "--datadir       Specify path to a data directory, overriding the default. This option can be used "
                 "multiple times to add multiple directories. If no data directories are given, the default is to use "
                 "(data/samples/mnist/, data/mnist/)"
              << std::endl;
    std::cout << "--useDLACore=N  Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, "
                 "where n is the number of DLA engines on the platform."
              << std::endl;
    std::cout << "--int8          Run in Int8 mode.\n";
    std::cout << "--fp16          Run in FP16 mode.\n";
}

int main(int argc, char** argv)
{
    samplesCommon::Args args;
    bool argsOK = samplesCommon::parseArgs(args, argc, argv);
    if (!argsOK)
    {
        sample::gLogError << "Invalid arguments" << std::endl;
        printHelpInfo();
        return EXIT_FAILURE;
    }
    if (args.help)
    {
        printHelpInfo();
        return EXIT_SUCCESS;
    }

    auto sampleTest = sample::gLogger.defineTest(gSampleName, argc, argv);

    sample::gLogger.reportTestStart(sampleTest);

    samplesCommon::CaffeSampleParams params = initializeSampleParams(args);

    // Write Algorithm Cache.
    SampleAlgorithmSelector sampleAlgorithmSelector(params);

    {
        sample::gLogInfo << "Building and running a GPU inference engine for MNIST." << std::endl;
        sample::gLogInfo << "Writing Algorithm Cache for MNIST." << std::endl;
        AlgorithmCacheWriter algorithmCacheWriter(gCacheFileName);

        if (!sampleAlgorithmSelector.build(&algorithmCacheWriter))
        {
            return sample::gLogger.reportFail(sampleTest);
        }

        if (!sampleAlgorithmSelector.infer())
        {
            return sample::gLogger.reportFail(sampleTest);
        }
    }

    {
        // Build network using Cache from previous run.
        sample::gLogInfo << "Building a GPU inference engine for MNIST using Algorithm Cache." << std::endl;
        AlgorithmCacheReader algorithmCacheReader(gCacheFileName);

        if (!sampleAlgorithmSelector.build(&algorithmCacheReader))
        {
            return sample::gLogger.reportFail(sampleTest);
        }

        if (!sampleAlgorithmSelector.infer())
        {
            return sample::gLogger.reportFail(sampleTest);
        }
    }

    {
        // Build network using MinimumWorkspaceAlgorithmSelector.
        sample::gLogInfo
            << "Building a GPU inference engine for MNIST using Algorithms with minimum workspace requirements."
            << std::endl;
        MinimumWorkspaceAlgorithmSelector minimumWorkspaceAlgorithmSelector;
        if (!sampleAlgorithmSelector.build(&minimumWorkspaceAlgorithmSelector))
        {
            return sample::gLogger.reportFail(sampleTest);
        }

        if (!sampleAlgorithmSelector.infer())
        {
            return sample::gLogger.reportFail(sampleTest);
        }
    }

    if (!sampleAlgorithmSelector.teardown())
    {
        return sample::gLogger.reportFail(sampleTest);
    }

    return sample::gLogger.reportPass(sampleTest);
}
