#include "pch.h"
#include "DlssNr_VkQueueProof.h"
#include "DlssNr_VkRecording.h"
#include <cassert>
#include <iostream>

using namespace DlssNr;
using namespace VkChain;
template <class T> T H(uint64_t value) { return (T) value; }
int main()
{
    VkDeviceQueueCreateInfo requests[2] {};
    for (auto& r : requests)
    {
        r.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        r.queueCount = 1;
    }
    requests[0].queueFamilyIndex = 1;
    requests[1].queueFamilyIndex = 2;
    VkDeviceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 2;
    info.pQueueCreateInfos = requests;
    const auto families = [](uint32_t family)
    {
        return std::pair<uint32_t, uint32_t> { family == 1 ? VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
                                                           : VK_QUEUE_TRANSFER_BIT,
                                               4 };
    };
    auto shape = DescribeQueues(&info, families);
    assert(shape.valid && shape.eligible == 1 && shape.family == 1);
    requests[0].queueCount = 2;
    shape = DescribeQueues(&info, families);
    assert(!shape.valid && shape.eligible == 2);
    requests[0].queueCount = 1;
    requests[1].queueFamilyIndex = 1;
    assert(!DescribeQueues(&info, families).valid);
    requests[1].queueFamilyIndex = 2;
    requests[0].flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    assert(!DescribeQueues(&info, families).valid);
    requests[0].flags = 0;
    assert(!DescribeQueues(nullptr, families).valid);
    assert(!DescribeQueues(&info, [](uint32_t) { return std::pair<uint32_t, uint32_t> { 0, 0 }; }).valid);
    shape = DescribeQueues(&info, families);
    QueueProof<> proof;
    proof.Configure(shape);
    proof.Remember(H<VkCommandPool>(1), 1);
    assert(!proof.Accept(H<VkCommandPool>(1)) && proof.Reason() == QueueRefusal::UnobservedQueue);
    assert(proof.Report() && !proof.Report());
    proof.Queue(H<VkQueue>(1), 1, 0);
    assert(!proof.Accept(H<VkCommandPool>(1))); // no upgrade after a blind interval
    proof.Configure(shape);
    proof.Queue(H<VkQueue>(1), 1, 0);
    proof.Remember(H<VkCommandPool>(1), 2);
    assert(!proof.Accept(H<VkCommandPool>(1)) && proof.Reason() == QueueRefusal::PoolFamily);
    proof.Configure(shape);
    proof.Queue(H<VkQueue>(1), 1, 0);
    proof.Remember(H<VkCommandPool>(1), 1);
    proof.Remember(H<VkCommandPool>(1), 1);
    assert(proof.Accept(H<VkCommandPool>(1)));
    proof.Forget(H<VkCommandPool>(1));
    assert(!proof.Accept(H<VkCommandPool>(1)) && proof.Reason() == QueueRefusal::UnobservedPool);
    proof.Configure(shape);
    proof.Queue(H<VkQueue>(1), 1, 0);
    for (uint64_t pool = 1; pool <= 128; ++pool)
        proof.Remember(H<VkCommandPool>(pool), 1);
    assert(proof.Accept(H<VkCommandPool>(128)));
    proof.Remember(H<VkCommandPool>(129), 1);
    assert(!proof.Accept(H<VkCommandPool>(1)) && proof.Reason() == QueueRefusal::Capacity);
    VkRecording::Registry registry;
    registry.Allocate(1, 2, { 3 }, VkRecording::Level::Primary);
    registry.Begin(3, VkRecording::Reuse::Serial, true);
    const auto ticket = *registry.Current(1, 3);
    assert(registry.PoolOf(ticket) == 2);
    registry.Reset(3);
    assert(!registry.PoolOf(ticket));
    registry.Begin(3, VkRecording::Reuse::Serial, true);
    assert(!registry.PoolOf(ticket));
    std::cout << "PASS: one eligible queue, pool provenance, no blind upgrade and bounded observation\n";
}
