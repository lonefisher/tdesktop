#include "core/update_failure.h"

#include <cassert>

int main() {
	using Core::UpdateFailure;
	using Core::UpdateRequestStage;
	assert(Core::FailureForRequestError(
		false) == UpdateFailure::Network);
	assert(Core::FailureForRequestError(true) == UpdateFailure::Timeout);
	assert(Core::FailureForInvalidResponse(
		UpdateRequestStage::Manifest) == UpdateFailure::Manifest);
	assert(Core::FailureForInvalidResponse(
		UpdateRequestStage::ManifestSignature) == UpdateFailure::Signature);
	assert(Core::FailureForInvalidResponse(
		UpdateRequestStage::Index) == UpdateFailure::Index);
	assert(Core::FailureForRequestError(true) == UpdateFailure::Timeout);
}
