#include <ATen/native/vulkan/api/Resource.h>
#include <ATen/native/vulkan/api/Adapter.h>

namespace at {
namespace native {
namespace vulkan {
namespace api {
namespace {

VmaAllocator create_allocator(
    const VkInstance instance,
    const VkPhysicalDevice physical_device,
    const VkDevice device) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      instance,
      "Invalid Vulkan instance!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      physical_device,
      "Invalid Vulkan physical device!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device,
      "Invalid Vulkan device!");

  const VmaAllocatorCreateInfo allocator_create_info{
    0u,
    physical_device,
    device,
    0u,
    nullptr,
    nullptr,
    1u,
    nullptr,
    nullptr,
    nullptr,
    instance,
    VK_API_VERSION_1_0,
  };

  VmaAllocator allocator{};
  VK_CHECK(vmaCreateAllocator(&allocator_create_info, &allocator));
  TORCH_CHECK(allocator, "Invalid VMA (Vulkan Memory Allocator) allocator!");

  return allocator;
}

VmaAllocationCreateInfo create_allocation_create_info(
    const Resource::Memory::Descriptor& descriptor) {
  return VmaAllocationCreateInfo{
    VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT,
    descriptor.usage,
    descriptor.required,
    descriptor.preferred,
    0u,
    VK_NULL_HANDLE,
    nullptr,
    0.5f,
  };
}

} // namespace

//
// VulkanBuffer
//

VulkanBuffer::VulkanBuffer()
  : memory_properties_{},
    buffer_properties_{},
    allocator_(VK_NULL_HANDLE),
    allocation_(VK_NULL_HANDLE),
    handle_(VK_NULL_HANDLE) {
}

VulkanBuffer::VulkanBuffer(
    const VmaAllocator vma_allocator,
    const VkDeviceSize size,
    const VulkanBuffer::MemoryProperties& mem_props)
  : memory_properties_(mem_props),
    buffer_properties_({
      size,
      0u,
      size,
    }),
    allocator_(vma_allocator),
    allocation_(VK_NULL_HANDLE),
    handle_(VK_NULL_HANDLE) {
  const VkBufferCreateInfo buffer_create_info{
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,  // sType
    nullptr,  // pNext
    0u,  // flags
    size,  // size
    memory_properties_.buffer_usage,  // usage
    VK_SHARING_MODE_EXCLUSIVE,  // sharingMode
    0u,  // queueFamilyIndexCount
    nullptr,  // pQueueFamilyIndices
  };

  // TODO: enable creation with a custom pool
  VmaAllocationCreateInfo alloc_create_info {
    VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT, // flags
    memory_properties_.memory_usage,  // usage
    memory_properties_.required_mem_flags,  // requiredFlags
    memory_properties_.preferred_mem_flags,  // preferredFlags
    0u,  // memoryTypeBits
    VK_NULL_HANDLE,  // pool
    nullptr,  // pUserData
    0.5f,  // priority
  };

  VK_CHECK(vmaCreateBuffer(
      allocator_, &buffer_create_info, &alloc_create_info,
      &handle_, &allocation_, nullptr));
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
  : memory_properties_(other.memory_properties_),
    buffer_properties_(other.buffer_properties_),
    allocator_(other.allocator_),
    allocation_(other.allocation_),
    handle_(other.handle_) {
  other.allocation_ = VK_NULL_HANDLE;
  other.handle_ = VK_NULL_HANDLE;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
  const VmaAllocation tmp_allocation = allocation_;
  const VkBuffer tmp_buffer = handle_;

  memory_properties_ = other.memory_properties_;
  buffer_properties_ = other.buffer_properties_;
  allocator_ = other.allocator_;
  allocation_ = other.allocation_;
  handle_ = other.handle_;

  other.allocation_ = tmp_allocation;
  other.handle_ = tmp_buffer;

  return *this;
}

VulkanBuffer::~VulkanBuffer() {
  if (VK_NULL_HANDLE != handle_) {
    vmaDestroyBuffer(allocator_, handle_, allocation_);
  }
}

//
// MemoryMap
//

MemoryMap::MemoryMap(const VulkanBuffer& buffer, const uint8_t access)
  : access_(access),
    allocator_(buffer.vma_allocator()),
    allocation_(buffer.allocation()),
    data_(nullptr) {
  VK_CHECK(vmaMapMemory(allocator_, allocation_, &data_));
}

MemoryMap::MemoryMap(MemoryMap&& other) noexcept
  : access_(other.access_),
    allocator_(other.allocator_),
    allocation_(other.allocation_),
    data_(other.data_) {
  other.allocation_ = VK_NULL_HANDLE;
  other.data_ = nullptr;
}

MemoryMap::~MemoryMap() {
  if (C10_UNLIKELY(!data_)) {
    return;
  }

  if (access_ & MemoryAccessType::WRITE) {
    // Call will be ignored by implementation if the memory type this allocation
    // belongs to is not HOST_VISIBLE or is HOST_COHERENT, which is the behavior
    // we want.
    VK_CHECK(vmaFlushAllocation(allocator_, allocation_, 0u, VK_WHOLE_SIZE));
  }

  vmaUnmapMemory(allocator_, allocation_);
}

void MemoryMap::invalidate() {
  if (access_ & MemoryAccessType::READ) {
    // Call will be ignored by implementation if the memory type this allocation
    // belongs to is not HOST_VISIBLE or is HOST_COHERENT, which is the behavior
    // we want.
    VK_CHECK(vmaInvalidateAllocation(
        allocator_, allocation_, 0u, VK_WHOLE_SIZE));
  }
}

//
// ImageSampler
//

bool operator==(
    const ImageSampler::Properties& _1,
    const ImageSampler::Properties& _2) {
  return (_1.filter == _2.filter && \
          _1.mipmap_mode == _2.mipmap_mode && \
          _1.address_mode == _2.address_mode && \
          _1.border_color == _2.border_color);
}

ImageSampler::ImageSampler(
    const VkDevice device,
    const ImageSampler::Properties& props)
  : device_(device),
    handle_(VK_NULL_HANDLE) {
  const VkSamplerCreateInfo sampler_create_info{
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,  // sType
    nullptr,  // pNext
    0u,  // flags
    props.filter,  // magFilter
    props.filter,  // minFilter
    props.mipmap_mode,  // mipmapMode
    props.address_mode,  // addressModeU
    props.address_mode,  // addressModeV
    props.address_mode,  // addressModeW
    0.0f,  // mipLodBias
    VK_FALSE,  // anisotropyEnable
    1.0f,  // maxAnisotropy,
    VK_FALSE,  // compareEnable
    VK_COMPARE_OP_NEVER,  // compareOp
    0.0f,  // minLod
    VK_LOD_CLAMP_NONE,  // maxLod
    props.border_color,  // borderColor
    VK_FALSE,  // unnormalizedCoordinates
  };

  VK_CHECK(vkCreateSampler(
      device_, &sampler_create_info, nullptr, &handle_));
}

ImageSampler::ImageSampler(ImageSampler&& other) noexcept
  : device_(other.device_),
    handle_(other.handle_) {
  other.handle_ = VK_NULL_HANDLE;
}

ImageSampler::~ImageSampler() {
  if C10_LIKELY(VK_NULL_HANDLE == handle_) {
    return;
  }
  vkDestroySampler(device_, handle_, nullptr);
}

size_t ImageSampler::Hasher::operator()(
    const ImageSampler::Properties& props) const {
  return c10::get_hash(
      props.filter,
      props.mipmap_mode,
      props.address_mode,
      props.border_color);
}

void swap(ImageSampler& lhs, ImageSampler& rhs) {
  VkDevice tmp_device = lhs.device_;
  VkSampler tmp_handle = lhs.handle_;

  lhs.device_ = rhs.device_;
  lhs.handle_ = rhs.handle_;

  rhs.device_ = tmp_device;
  rhs.handle_ = tmp_handle;
}

//
// VulkanImage
//

VulkanImage::VulkanImage()
  : memory_properties_{},
    image_properties_{},
    view_properties_{},
    sampler_properties_{},
    allocator_(VK_NULL_HANDLE),
    allocation_(VK_NULL_HANDLE),
    handles_{
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
    },
    layout_{} {
}

VulkanImage::VulkanImage(
    const VmaAllocator vma_allocator,
    const VkDevice device,
    const MemoryProperties& mem_props,
    const ImageProperties& image_props,
    const ViewProperties& view_props,
    const SamplerProperties& sampler_props,
    const VkImageLayout layout,
    const VkSampler sampler)
  : memory_properties_(mem_props),
    image_properties_(image_props),
    view_properties_(view_props),
    sampler_properties_(sampler_props),
    allocator_(vma_allocator),
    allocation_(VK_NULL_HANDLE),
    handles_{
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        sampler,
    },
    layout_(layout) {
  const VkImageCreateInfo image_create_info{
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,  // sType
    nullptr,  // pNext
    0u,  // flags
    image_properties_.image_type,  // imageType
    image_properties_.image_format,  // format
    image_properties_.image_extents,  // extents
    1u,  // mipLevels
    1u,  // arrayLayers
    VK_SAMPLE_COUNT_1_BIT,  // samples
    VK_IMAGE_TILING_OPTIMAL,  // tiling
    memory_properties_.image_usage,  // usage
    VK_SHARING_MODE_EXCLUSIVE,  // sharingMode
    0u,  // queueFamilyIndexCount
    nullptr,  // pQueueFamilyIndices
    layout_,  // initialLayout
  };

  // TODO: enable creation with a custom pool
  const VmaAllocationCreateInfo alloc_create_info{
    VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT, // flags
    memory_properties_.memory_usage,  // usage
    memory_properties_.required_mem_flags,  // requiredFlags
    memory_properties_.preferred_mem_flags,  // preferredFlags
    0u,  // memoryTypeBits
    VK_NULL_HANDLE,  // pool
    nullptr,  // pUserData
    0.5f,  // priority
  };

  VK_CHECK(vmaCreateImage(
      allocator_, &image_create_info, &alloc_create_info,
      &(handles_.image), &allocation_, nullptr));

  // Image View

  const VkComponentMapping component_mapping{
    VK_COMPONENT_SWIZZLE_IDENTITY,  // r
    VK_COMPONENT_SWIZZLE_IDENTITY,  // g
    VK_COMPONENT_SWIZZLE_IDENTITY,  // b
    VK_COMPONENT_SWIZZLE_IDENTITY,  // a
  };

  const VkImageSubresourceRange subresource_range{
    VK_IMAGE_ASPECT_COLOR_BIT,  // aspectMask
    0u,  // baseMipLevel
    VK_REMAINING_MIP_LEVELS,  // levelCount
    0u,  // baseArrayLayer
    VK_REMAINING_ARRAY_LAYERS,  // layerCount
  };

  const VkImageViewCreateInfo image_view_create_info{
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,  // sType
    nullptr,  // pNext
    0u,  // flags
    handles_.image,  // image
    view_properties_.view_type,  // viewType
    view_properties_.view_format,  // format
    component_mapping,  // components
    subresource_range,  // subresourceRange
  };

  VK_CHECK(vkCreateImageView(
      device, &image_view_create_info, nullptr, &(handles_.image_view)));
}

VulkanImage::VulkanImage(VulkanImage&& other) noexcept
  : memory_properties_(other.memory_properties_),
    image_properties_(other.image_properties_),
    view_properties_(other.view_properties_),
    sampler_properties_(other.sampler_properties_),
    allocator_(other.allocator_),
    allocation_(other.allocation_),
    handles_(other.handles_),
    layout_(other.layout_) {
  other.allocation_ = VK_NULL_HANDLE;
  other.handles_.image = VK_NULL_HANDLE;
  other.handles_.image_view = VK_NULL_HANDLE;
  other.handles_.sampler = VK_NULL_HANDLE;
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept {
  const VmaAllocation tmp_allocation = allocation_;
  const VkImage tmp_image = handles_.image;
  const VkImageView tmp_image_view = handles_.image_view;

  memory_properties_ = other.memory_properties_;
  image_properties_ = other.image_properties_;
  view_properties_ = other.view_properties_;
  sampler_properties_ = other.sampler_properties_;
  allocator_ = other.allocator_;
  allocation_ = other.allocation_;
  handles_ = other.handles_;
  layout_ = other.layout_;

  other.allocation_ = tmp_allocation;
  other.handles_.image = tmp_image;
  other.handles_.image_view = tmp_image_view;

  return *this;
}

VulkanImage::~VulkanImage() {
  if (VK_NULL_HANDLE != handles_.image_view) {
    VmaAllocatorInfo allocator_info{};
    vmaGetAllocatorInfo(allocator_, &allocator_info);
    vkDestroyImageView(allocator_info.device, handles_.image_view, nullptr);
  }

  if (VK_NULL_HANDLE != handles_.image) {
    vmaDestroyImage(allocator_, handles_.image, allocation_);
  }
}

//
// SamplerCache
//

SamplerCache::SamplerCache(const VkDevice device)
  : cache_mutex_{},
    device_(device),
    cache_{} {
}

SamplerCache::SamplerCache(SamplerCache&& other) noexcept
  : cache_mutex_{},
    device_(other.device_) {
  std::lock_guard<std::mutex> lock(other.cache_mutex_);
  cache_ = std::move(other.cache_);
}

SamplerCache::~SamplerCache() {
  purge();
}

VkSampler SamplerCache::retrieve(const SamplerCache::Key& key) {
  std::lock_guard<std::mutex> lock(cache_mutex_);

  auto it = cache_.find(key);
  if C10_UNLIKELY(cache_.cend() == it) {
    it = cache_.insert({key, SamplerCache::Value(device_, key)}).first;
  }

  return it->second.handle();
}

void SamplerCache::purge() {
  cache_.clear();
}

//
// MemoryAllocator
//

MemoryAllocator::MemoryAllocator(
    const VkInstance instance,
    const VkPhysicalDevice physical_device,
    const VkDevice device)
  : instance_{},
    physical_device_(physical_device),
    device_(device) {
  const VmaAllocatorCreateInfo allocator_create_info{
    0u,  // flags
    physical_device_,  // physicalDevice
    device_,  // device
    0u,  // preferredLargeHeapBlockSize
    nullptr,  // pAllocationCallbacks
    nullptr,  // pDeviceMemoryCallbacks
    1u,  // frameinUseCount
    nullptr,  // pHeapSizeLimit
    nullptr,  // pVulkanFunctions
    nullptr,  // pRecordSettings
    instance,  // instance
    VK_API_VERSION_1_0,  // vulkanApiVersion
  };

  VK_CHECK(vmaCreateAllocator(&allocator_create_info, &allocator_));
}

MemoryAllocator::MemoryAllocator(MemoryAllocator&& other) noexcept
  : instance_(other.instance_),
    physical_device_(other.physical_device_),
    device_(other.device_),
    allocator_(other.allocator_) {
  other.allocator_ = VK_NULL_HANDLE;
  other.device_ = VK_NULL_HANDLE;
  other.physical_device_ = VK_NULL_HANDLE;
  other.instance_ = VK_NULL_HANDLE;
}

MemoryAllocator::~MemoryAllocator() {
  if C10_LIKELY(VK_NULL_HANDLE == allocator_) {
    return;
  }
  vmaDestroyAllocator(allocator_);
}

VulkanImage MemoryAllocator::create_image3d_fp(
      const VkExtent3D& extents,
      const VulkanImage::SamplerProperties& sampler_props,
      const VkSampler sampler,
      bool allow_transfer) {
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  if (allow_transfer) {
    usage |= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  }

  const VulkanImage::MemoryProperties mem_props{
    VMA_MEMORY_USAGE_GPU_ONLY,
    0u,
    0u,
    usage,
  };

#ifdef USE_VULKAN_FP16_INFERENCE
    const VkFormat image_format = VK_FORMAT_R16G16B16A16_SFLOAT;
#else
    const VkFormat image_format = VK_FORMAT_R32G32B32A32_SFLOAT;
#endif

  const VulkanImage::ImageProperties image_props{
    VK_IMAGE_TYPE_3D,
    image_format,
    extents,
  };

  const VulkanImage::ViewProperties view_props{
    VK_IMAGE_VIEW_TYPE_3D,
    image_format,
  };

  const VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  return VulkanImage(
      allocator_, device_,
      mem_props, image_props, view_props, sampler_props,
      initial_layout, sampler);
}

VulkanBuffer MemoryAllocator::create_storage_buffer(
    const VkDeviceSize size, const bool gpu_only) {
  const VkBufferUsageFlags buffer_usage = \
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  const VmaMemoryUsage vma_usage = \
      gpu_only ? VMA_MEMORY_USAGE_GPU_ONLY : VMA_MEMORY_USAGE_GPU_TO_CPU;

  const VkMemoryPropertyFlags preferred_mem_props = \
      gpu_only ? 0u : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  const VulkanBuffer::MemoryProperties mem_props{
    vma_usage,
    0u,
    preferred_mem_props,
    buffer_usage,
  };

  return VulkanBuffer(allocator_, size, mem_props);
}

VulkanBuffer MemoryAllocator::create_staging_buffer(const VkDeviceSize size) {
  const VulkanBuffer::MemoryProperties mem_props{
    VMA_MEMORY_USAGE_CPU_COPY,
    0u,
    0u,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };

  return VulkanBuffer(allocator_, size, mem_props);
}

//
// VulkanFence
//

VulkanFence::VulkanFence()
  : device_(VK_NULL_HANDLE),
    handle_(VK_NULL_HANDLE),
    waiting_(false) {
}

VulkanFence::VulkanFence(const VkDevice device)
  : device_(device),
    handle_(VK_NULL_HANDLE),
    waiting_(VK_NULL_HANDLE) {
  const VkFenceCreateInfo fence_create_info{
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,  // sType
    nullptr,  // pNext
    0u,  // flags
  };

  VK_CHECK(vkCreateFence(
      device_,
      &fence_create_info,
      nullptr,
      &handle_));
}

VulkanFence::VulkanFence(VulkanFence&& other) noexcept
  : device_(other.device_),
    handle_(other.handle_),
    waiting_(other.waiting_) {
  other.handle_ = VK_NULL_HANDLE;
  other.waiting_ = false;
}

VulkanFence& VulkanFence::operator=(VulkanFence&& other) noexcept {
  device_ = other.device_;
  handle_ = other.handle_;
  waiting_ = other.waiting_;

  other.device_ = VK_NULL_HANDLE;
  other.handle_ = VK_NULL_HANDLE;
  other.waiting_ = false;

  return *this;
}

VulkanFence::~VulkanFence() {
  if C10_LIKELY(VK_NULL_HANDLE == handle_) {
    return;
  }
  vkDestroyFence(device_, handle_, nullptr);
}

void VulkanFence::wait() {
  // if get_submit_handle() has not been called, then this will no-op
  if (waiting_) {
    VK_CHECK(vkWaitForFences(
        device_,
        1u,
        &handle_,
        VK_TRUE,
        UINT64_MAX));

    VK_CHECK(vkResetFences(
        device_,
        1u,
        &handle_));

    waiting_ = false;
  }
}

/* ----- Old Code --- */


void release_buffer(const Resource::Buffer& buffer) {
  // Safe to pass null as buffer or allocation.
  vmaDestroyBuffer(
      buffer.memory.allocator,
      buffer.object.handle,
      buffer.memory.allocation);
}

void release_image(const Resource::Image& image) {
  // Sampler is an immutable object. Its lifetime is managed through the cache.

  if (VK_NULL_HANDLE != image.object.view) {
    VmaAllocatorInfo allocator_info{};
    vmaGetAllocatorInfo(image.memory.allocator, &allocator_info);
    vkDestroyImageView(allocator_info.device, image.object.view, nullptr);
  }

  // Safe to pass null as image or allocation.
  vmaDestroyImage(
      image.memory.allocator,
      image.object.handle,
      image.memory.allocation);
}

void* map(
    const Resource::Memory& memory,
    const Resource::Memory::Access::Flags access) {
  void* data = nullptr;
  VK_CHECK(vmaMapMemory(memory.allocator, memory.allocation, &data));

  if (access & Resource::Memory::Access::Read) {
    // Call will be ignored by implementation if the memory type this allocation
    // belongs to is not HOST_VISIBLE or is HOST_COHERENT, which is the behavior
    // we want.
    VK_CHECK(vmaInvalidateAllocation(
        memory.allocator, memory.allocation, 0u, VK_WHOLE_SIZE));
  }

  return data;
}

Resource::Memory::Scope::Scope(
    const VmaAllocator allocator,
    const VmaAllocation allocation,
    const Access::Flags access)
  : allocator_(allocator),
    allocation_(allocation),
    access_(access) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      allocator,
      "Invalid VMA (Vulkan Memory Allocator) allocator!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      allocation,
      "Invalid VMA (Vulkan Memory Allocator) allocation!");
}

void Resource::Memory::Scope::operator()(const void* const data) const {
  if (C10_UNLIKELY(!data)) {
    return;
  }

  if (access_ & Access::Write) {
    // Call will be ignored by implementation if the memory type this allocation
    // belongs to is not HOST_VISIBLE or is HOST_COHERENT, which is the behavior
    // we want.
    VK_CHECK(vmaFlushAllocation(allocator_, allocation_, 0u, VK_WHOLE_SIZE));
  }

  vmaUnmapMemory(allocator_, allocation_);
}

Resource::Image::Sampler::Factory::Factory(const GPU& gpu)
  : device_(gpu.device) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_,
      "Invalid Vulkan device!");
}

typename Resource::Image::Sampler::Factory::Handle
Resource::Image::Sampler::Factory::operator()(
    const Descriptor& descriptor) const {
  const VkSamplerCreateInfo sampler_create_info{
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    nullptr,
    0u,
    descriptor.filter,
    descriptor.filter,
    descriptor.mipmap_mode,
    descriptor.address_mode,
    descriptor.address_mode,
    descriptor.address_mode,
    0.0f,
    VK_FALSE,
    1.0f,
    VK_FALSE,
    VK_COMPARE_OP_NEVER,
    0.0f,
    VK_LOD_CLAMP_NONE,
    descriptor.border,
    VK_FALSE,
  };

  VkSampler sampler{};
  VK_CHECK(vkCreateSampler(
      device_,
      &sampler_create_info,
      nullptr,
      &sampler));

  TORCH_CHECK(
      sampler,
      "Invalid Vulkan image sampler!");

  return Handle{
    sampler,
    Deleter(device_),
  };
}

VkFence Resource::Fence::handle(const bool add_to_waitlist) const {
  if (!pool) {
    return VK_NULL_HANDLE;
  }

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      id < pool->fence_.pool.size(),
      "Invalid Vulkan fence!");

  const VkFence fence = pool->fence_.pool[id].get();

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      fence,
      "Invalid Vulkan fence!");

  if (add_to_waitlist) {
    pool->fence_.waitlist.push_back(fence);
  }

  return fence;
}

void Resource::Fence::wait(const uint64_t timeout_nanoseconds) {
  const VkFence fence = handle(/* add_to_waitlist = */ false);

  const auto waitlist_itr = std::find(
      pool->fence_.waitlist.cbegin(),
      pool->fence_.waitlist.cend(),
      fence);

  if (pool->fence_.waitlist.cend() != waitlist_itr) {
    VK_CHECK(vkWaitForFences(
        pool->device_,
        1u,
        &fence,
        VK_TRUE,
        timeout_nanoseconds));

    VK_CHECK(vkResetFences(
        pool->device_,
        1u,
        &fence));

    pool->fence_.waitlist.erase(waitlist_itr);
  }
}

namespace {

class Linear final : public Resource::Pool::Policy {
 public:
  Linear(
      VkDeviceSize block_size,
      uint32_t min_block_count,
      uint32_t max_block_count);

  virtual void enact(
      VmaAllocator allocator,
      const VkMemoryRequirements& memory_requirements,
      VmaAllocationCreateInfo& allocation_create_info) override;

 private:
  struct Configuration final {
    static constexpr uint32_t kReserve = 16u;
  };

  struct Entry final {
    class Deleter final {
     public:
      explicit Deleter(VmaAllocator);
      void operator()(VmaPool) const;

     private:
      VmaAllocator allocator_;
    };

    uint32_t memory_type_index;
    Handle<VmaPool, Deleter> handle;
  };

  std::vector<Entry> pools_;

  struct {
    VkDeviceSize size;
    uint32_t min;
    uint32_t max;
  } block_;
};

Linear::Entry::Deleter::Deleter(const VmaAllocator allocator)
  : allocator_(allocator) {
}

void Linear::Entry::Deleter::operator()(const VmaPool pool) const {
  vmaDestroyPool(allocator_, pool);
}

Linear::Linear(
    const VkDeviceSize block_size,
    const uint32_t min_block_count,
    const uint32_t max_block_count)
  : block_ {
      block_size,
      min_block_count,
      max_block_count,
    } {
  pools_.reserve(Configuration::kReserve);
}

void Linear::enact(
    const VmaAllocator allocator,
    const VkMemoryRequirements& memory_requirements,
    VmaAllocationCreateInfo& allocation_create_info) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      allocator,
      "Invalid VMA (Vulkan Memory Allocator) allocator!");

  uint32_t memory_type_index = 0u;
  VK_CHECK(vmaFindMemoryTypeIndex(
      allocator,
      memory_requirements.memoryTypeBits,
      &allocation_create_info,
      &memory_type_index));

  auto pool_itr = std::find_if(
      pools_.begin(),
      pools_.end(),
      [memory_type_index](const Entry& entry) {
    return entry.memory_type_index == memory_type_index;
  });

  if (pools_.end() == pool_itr) {
    const VmaPoolCreateInfo pool_create_info{
      memory_type_index,
      VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT,
      block_.size,
      block_.min,
      block_.max,
      0u,
    };

    VmaPool pool{};
    VK_CHECK(vmaCreatePool(
        allocator,
        &pool_create_info,
        &pool));

    TORCH_CHECK(
        pool,
        "Invalid VMA (Vulkan Memory Allocator) memory pool!");

    pools_.push_back({
      memory_type_index,
      {
        pool,
        Entry::Deleter(allocator),
      },
    });

    pool_itr = std::prev(pools_.end());
  }

  allocation_create_info.pool = pool_itr->handle.get();
}

} // namespace

std::unique_ptr<Resource::Pool::Policy> Resource::Pool::Policy::linear(
    const VkDeviceSize block_size,
    const uint32_t min_block_count,
    const uint32_t max_block_count) {
  return std::make_unique<Linear>(
      block_size,
      min_block_count,
      max_block_count);
}

Resource::Pool::Pool(
    const GPU& gpu,
    std::unique_ptr<Policy> policy)
  : device_(gpu.device),
    allocator_(
        create_allocator(
            gpu.instance,
            gpu.adapter->physical_handle(),
            device_),
        vmaDestroyAllocator),
    memory_{
      std::move(policy),
    },
    image_{
      .sampler = Image::Sampler{gpu},
    },
    fence_{} {
  buffer_.pool.reserve(Configuration::kReserve);
  image_.pool.reserve(Configuration::kReserve);
  fence_.pool.reserve(Configuration::kReserve);
}

Resource::Pool::Pool(Pool&& pool)
  : device_(std::move(pool.device_)),
    allocator_(std::move(pool.allocator_)),
    memory_(std::move(pool.memory_)),
    buffer_(std::move(pool.buffer_)),
    image_(std::move(pool.image_)),
    fence_(std::move(pool.fence_)) {
  pool.invalidate();
}

Resource::Pool& Resource::Pool::operator=(Pool&& pool) {
  if (&pool != this) {
    device_ = std::move(pool.device_);
    allocator_ = std::move(pool.allocator_);
    memory_ = std::move(pool.memory_);
    buffer_ = std::move(pool.buffer_);
    image_ = std::move(pool.image_);
    fence_ = std::move(pool.fence_);

    pool.invalidate();
  };

  return *this;
}

Resource::Pool::~Pool() {
  try {
    if (device_ && allocator_) {
      purge();
    }
  }
  catch (const std::exception& e) {
    TORCH_WARN(
        "Vulkan: Resource pool destructor raised an exception! Error: ",
        e.what());
  }
  catch (...) {
    TORCH_WARN(
        "Vulkan: Resource pool destructor raised an exception! "
        "Error: Unknown");
  }
}

Resource::Buffer Resource::Pool::create_buffer(
    const Buffer::Descriptor& descriptor) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_ && allocator_,
      "This resource pool is in an invalid state! ",
      "Potential reason: This resource pool is moved from.");

  const VkBufferCreateInfo buffer_create_info{
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    nullptr,
    0u,
    descriptor.size,
    descriptor.usage.buffer,
    VK_SHARING_MODE_EXCLUSIVE,
    0u,
    nullptr,
  };

  VkBuffer buffer{};
  VK_CHECK(vkCreateBuffer(
      device_,
      &buffer_create_info,
      nullptr,
      &buffer));

  TORCH_CHECK(
      buffer,
      "Invalid Vulkan buffer!");

  VkMemoryRequirements memory_requirements{};
  vkGetBufferMemoryRequirements(
      device_,
      buffer,
      &memory_requirements);

  VmaAllocationCreateInfo allocation_create_info =
      create_allocation_create_info(descriptor.usage.memory);

  if (memory_.policy) {
    memory_.policy->enact(
        allocator_.get(),
        memory_requirements,
        allocation_create_info);
  }

  VmaAllocation allocation{};
  VK_CHECK(vmaAllocateMemory(
      allocator_.get(),
      &memory_requirements,
      &allocation_create_info,
      &allocation,
      nullptr));

  TORCH_CHECK(
      allocation,
      "Invalid VMA (Vulkan Memory Allocator) allocation!");

  VK_CHECK(vmaBindBufferMemory(
      allocator_.get(),
      allocation,
      buffer));

  return Buffer{
    Buffer::Object{
      buffer,
      0u,
      descriptor.size,
    },
    Memory{
      allocator_.get(),
      allocation,
    },
  };
}

void Resource::Pool::register_buffer_cleanup(const Resource::Buffer& buffer) {
  buffer_.pool.emplace_back(buffer, &release_buffer);
}

Resource::Image Resource::Pool::create_image(
    const Image::Descriptor& descriptor) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_ && allocator_,
      "This resource pool is in an invalid state! ",
      "Potential reason: This resource pool is moved from.");

  const VkImageCreateInfo image_create_info{
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    nullptr,
    0u,
    descriptor.type,
    descriptor.format,
    descriptor.extent,
    1u,
    1u,
    VK_SAMPLE_COUNT_1_BIT,
    VK_IMAGE_TILING_OPTIMAL,
    descriptor.usage.image,
    VK_SHARING_MODE_EXCLUSIVE,
    0u,
    nullptr,
    VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkImage image{};
  VK_CHECK(vkCreateImage(
      device_,
      &image_create_info,
      nullptr,
      &image));

  TORCH_CHECK(
      image,
      "Invalid Vulkan image!");

  VkMemoryRequirements memory_requirements{};
  vkGetImageMemoryRequirements(
      device_,
      image,
      &memory_requirements);

  VmaAllocationCreateInfo allocation_create_info =
      create_allocation_create_info(descriptor.usage.memory);

  if (memory_.policy) {
    memory_.policy->enact(
        allocator_.get(),
        memory_requirements,
        allocation_create_info);
  }

  VmaAllocation allocation{};
  VK_CHECK(vmaAllocateMemory(
      allocator_.get(),
      &memory_requirements,
      &allocation_create_info,
      &allocation,
      nullptr));

  TORCH_CHECK(
      allocation,
      "Invalid VMA (Vulkan Memory Allocator) allocation!");

  VK_CHECK(vmaBindImageMemory(
      allocator_.get(),
      allocation,
      image));

  const VkImageViewCreateInfo image_view_create_info{
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    nullptr,
    0u,
    image,
    descriptor.view.type,
    descriptor.view.format,
    {
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
    },
    {
      VK_IMAGE_ASPECT_COLOR_BIT,
      0u,
      VK_REMAINING_MIP_LEVELS,
      0u,
      VK_REMAINING_ARRAY_LAYERS,
    },
  };

  VkImageView view{};
  VK_CHECK(vkCreateImageView(
      device_,
      &image_view_create_info,
      nullptr,
      &view));

  TORCH_CHECK(
      view,
      "Invalid Vulkan image view!");

  return Image{
    Image::Object{
      image,
      VK_IMAGE_LAYOUT_UNDEFINED,
      view,
      image_.sampler.cache.retrieve(descriptor.sampler),
    },
    Memory{
      allocator_.get(),
      allocation,
    },
  };
}

void Resource::Pool::register_image_cleanup(const Resource::Image& image) {
  image_.pool.emplace_back(image, &release_image);
}

Resource::Fence Resource::Pool::fence() {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_ && allocator_,
      "This resource pool is in an invalid state! ",
      "Potential reason: This resource pool is moved from.");

  if (fence_.pool.size() == fence_.in_use) {
    const VkFenceCreateInfo fence_create_info{
      VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      nullptr,
      0u,
    };

    VkFence fence{};
    VK_CHECK(vkCreateFence(
        device_,
        &fence_create_info,
        nullptr,
        &fence));

    TORCH_CHECK(
        fence,
        "Invalid Vulkan fence!");

    fence_.pool.emplace_back(fence, VK_DELETER(Fence)(device_));
  }

  return Fence{
    this,
    fence_.in_use++,
  };
}

void Resource::Pool::purge() {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_ && allocator_,
      "This resource pool is in an invalid state! ",
      "Potential reason: This resource pool is moved from.");

  if (!fence_.waitlist.empty()) {
    VK_CHECK(vkWaitForFences(
        device_,
        fence_.waitlist.size(),
        fence_.waitlist.data(),
        VK_TRUE,
        UINT64_MAX));

    VK_CHECK(vkResetFences(
        device_,
        fence_.waitlist.size(),
        fence_.waitlist.data()));

    fence_.waitlist.clear();
  }

  fence_.in_use = 0u;
  image_.pool.clear();
  buffer_.pool.clear();
}

void Resource::Pool::invalidate() {
  device_ = VK_NULL_HANDLE;
  allocator_.reset();
}

} // namespace api
} // namespace vulkan
} // namespace native
} // namespace at
