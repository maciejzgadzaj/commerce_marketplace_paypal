<?php
/**
 * @file
 * Includes generic functionality specific to PayPal Adaptive Payments.
 *
 * PayPal Adaptive Payments How-To's:
 * - general info:
 *   https://developer.paypal.com/docs/classic/adaptive-payments/gs_AdaptivePayments/
 *   https://developer.paypal.com/docs/classic/api/adaptive-payments/Pay_API_Operation/
 * - parallel payments:
 *   https://developer.paypal.com/docs/classic/adaptive-payments/ht_ap-parallelPayment-curl-etc/
 * - chained payment:
 *   https://developer.paypal.com/docs/classic/adaptive-payments/ht_ap-basicChainedPayment-curl-etc/
 * - IPNs:
 *   https://developer.paypal.com/docs/classic/adaptive-payments/integration-guide/APIPN/
 */

/**
 * Returns the URL to the specified PayPal Adaptive Payments server.
 *
 * @param $server
 *   Either sandbox or live indicating which server to get the URL for.
 *
 * @return
 *   The URL to use to submit requests to the PayPal Adaptive Payments server.
 */
function commerce_marketplace_paypal_wps_adaptive_server_url($server) {
  switch ($server) {
    case 'sandbox':
      return 'https://svcs.sandbox.paypal.com/AdaptivePayments/Pay';
    case 'live':
      return 'https://svcs.paypal.com/AdaptivePayments/Pay';
  }
}

/**
 * Returns Adaptive Payments command that identifies the flow.
 *
 * @param $payment_action
 * @return string
 *
 * @see commerce_marketplace_paypal_wps_form_commerce_paypal_wps_redirect_form_alter()
 * @see https://developer.paypal.com/docs/classic/adaptive-payments/integration-guide/APCommands/
 */
function commerce_marketplace_paypal_wps_adaptive_command($payment_action) {
  switch ($payment_action) {
    case 'sale':
      return '_ap-payment';
    case 'authorization':
      return '_ap-preapproval';
  }
}

/**
 * Returns the default Adaptive Payments settings for the PayPal WPS payment
 * method.
 */
function commerce_marketplace_paypal_wps_adaptive_default_settings() {
  return array(
    'marketplace_adaptive' => array(
      'payment_type' => 'parallel',
      'application_id' => '',
      'api_username' => '',
      'api_password' => '',
      'api_signature' => '',
      'reverse_all_parallel_payments_on_error' => 'true',
      'api_logging' => array(),
    ),
  );
}

/**
 * @see commerce_marketplace_paypal_wps_form_commerce_paypal_wps_redirect_form_alter()
 *
 * @see commerce_paypal_chained_get_paykey()
 * @see commerce_paypal_chained_order_form()
 * @see commerce_paypal_chained_redirect_form()
 */
function commerce_marketplace_paypal_wps_adaptive_get_paykey($form, $form_state) {
  $order = $form_state['build_info']['args'][0];
  $payment_method = $form_state['build_info']['args'][1];

  // Populate receivers array from all orders in the order group.
  $order_numbers = $receivers = array();
  $group_orders = commerce_order_load_multiple(NULL, array('order_group' => $order->order_group), TRUE);

  // For chained payments.
//  if ($payment_method['settings']['marketplace_adaptive']['payment_type'] == 'chained') {
//    $receivers[] = array(
//      'amount' => commerce_marketplace_order_calculate_total($group_orders),
//      'invoiceId' => commerce_marketplace_paypal_ipn_invoice($group_orders),
//      'email' => $payment_method['settings']['business'],
//      'primary' => 'true',
//    );
//  }

  foreach ($group_orders as $group_order) {
    $order_numbers[$group_order->order_id] = $group_order->order_number;

    $group_order_wrapper = entity_metadata_wrapper('commerce_order', $group_order);

    $receiver = array(
      'amount' => commerce_currency_amount_to_decimal($group_order_wrapper->commerce_order_total->amount->value(), $group_order_wrapper->commerce_order_total->currency_code->value()),
      'invoiceId' => commerce_paypal_ipn_invoice($group_order),
    );

    if ($store = $group_order_wrapper->commerce_store->value()) {
      $store_settings = $store->data['payment_methods']['settings'][$payment_method['method_id']];
      $receiver['email'] = $store_settings['business'];
    }
    else {
      $receiver['email'] = $payment_method['settings']['business'];
    }

    // For chained payments.
//    if ($payment_method['settings']['marketplace_adaptive']['payment_type'] == 'chained') {
//      $receiver['primary'] = 'false';
//    }

    $receivers[] = $receiver;
  }

  // Build the PAY request.
  // See https://developer.paypal.com/docs/classic/api/adaptive-payments/Pay_API_Operation/
  // for information on all possible request fields.
  $pay_request = array(
    'actionType' => 'PAY',
    'clientDetails' => array(
      'applicationId' => $payment_method['settings']['marketplace_adaptive']['application_id'],
    ),
    'currencyCode' => $form['currency_code']['#value'],
    'receiverList' => array(
      'receiver' => $receivers,
    ),
    'requestEnvelope' => array(
      'errorLanguage' => $form['lc']['#value'],
    ),
    'memo' => format_plural(count($group_orders),
      'Order @order_numbers at @store',
      'Orders @order_numbers at @store',
      array(
        '@order_numbers' => implode(', ', $order_numbers),
        '@store' => variable_get('site_name', url('<front>', array('absolute' => TRUE))),
      )
    ),
    'trackingId' => commerce_paypal_ipn_invoice($order),
    'cancelUrl' => $form['cancel_return']['#value'],
    'returnUrl' => $form['return']['#value'],
    'ipnNotificationUrl' => $form['notify_url']['#value'],
    'reverseAllParallelPaymentsOnError' => $payment_method['settings']['marketplace_adaptive']['reverse_all_parallel_payments_on_error'],
  );
//  dpm($pay_request, '$pay_request');

  // Log the request if logging is enabled.
  if (!empty($payment_method['settings']['marketplace_adaptive']['api_logging']['request'])) {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: pay key request: !request', array(
      '!request' => '<pre>' . var_export($pay_request, TRUE) . '</pre>',
    ), WATCHDOG_DEBUG);
  }

  // Convert it to JSON
  $pay_request_json = json_encode($pay_request);

  // Initialize cURL with our desired API endpoint
  $url = commerce_marketplace_paypal_wps_adaptive_server_url($payment_method['settings']['server']);
  $ch = curl_init($url);

  curl_setopt($ch, CURLOPT_CUSTOMREQUEST, 'POST');
  curl_setopt($ch, CURLOPT_RETURNTRANSFER, TRUE);
  curl_setopt($ch, CURLOPT_POSTFIELDS, $pay_request_json);

  $headers = array(
    'Content-Type: application/json',
    'Content-Length: ' . strlen($pay_request_json),
    'X-PAYPAL-SECURITY-USERID: ' . $payment_method['settings']['marketplace_adaptive']['api_username'],
    'X-PAYPAL-SECURITY-PASSWORD: ' . $payment_method['settings']['marketplace_adaptive']['api_password'],
    'X-PAYPAL-SECURITY-SIGNATURE: ' . $payment_method['settings']['marketplace_adaptive']['api_signature'],
    'X-PAYPAL-REQUEST-DATA-FORMAT: JSON',
    'X-PAYPAL-RESPONSE-DATA-FORMAT: JSON',
    'X-PAYPAL-APPLICATION-ID: ' . $payment_method['settings']['marketplace_adaptive']['application_id'],
  );
  curl_setopt($ch, CURLOPT_HTTPHEADER, $headers);

  // Execute the request.
  $result = curl_exec($ch);

  // Log the cURL response if logging is enabled.
  if (!empty($payment_method['settings']['marketplace_adaptive']['api_logging']['response'])) {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: pay key cURL response: !response', array(
      '!response' => '<pre>' . var_export($result, TRUE) . '</pre>',
    ), WATCHDOG_DEBUG);
  }

  // Log any errors to the watchdog.
  if ($error = curl_error($ch)) {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: cURL error: @error', array('@error' => $error), WATCHDOG_ERROR);
    return FALSE;
  }
  curl_close($ch);

  // Decode JSON response.
  $result = json_decode($result);
//  dpm($result, '$result');

  // Log the response if logging is enabled.
  if (!empty($payment_method['settings']['marketplace_adaptive']['api_logging']['response'])) {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: pay key response: !response', array(
      '!response' => '<pre>' . var_export($result, TRUE) . '</pre>',
    ), WATCHDOG_DEBUG);
  }

  // If everything was fine, just return received pay key.
  if (strtolower($result->responseEnvelope->ack) == 'success' && !empty($result->payKey)) {
    // Before saving received pay key, make sure there are no old pay chains
    // for the current order still existing in the database.
    db_delete('commerce_marketplace_paypal_pay_chain')
      ->condition('order_group', $order->order_group)
      ->execute();

    // Store the new pay chain.
    $pay_chain = array(
      'order_group' => $order->order_group,
      'pay_key' => $result->payKey,
      'currency' => $form['currency_code']['#value'],
      'data' => $receivers,
    );
    drupal_write_record('commerce_marketplace_paypal_pay_chain', $pay_chain);

    return $result->payKey;
  }
  // Otherwise log an error to the watchdog.
  else {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: failed to retrieve the pay key for order @order_number. PayPal response: !response', array(
      '@order_number' => $order->order_number,
      '!response' => '<pre>'. check_plain(print_r($result, TRUE)) .'</pre>'
    ), WATCHDOG_ERROR);
    return FALSE;
  }
}

/**
 * Payment method callback: process an IPN once it's been validated.
 *
 * Overrides commerce_paypal_wps_paypal_ipn_process()
 *
 * @see commerce_marketplace_paypal_wps_paypal_ipn_process()
 * @see commerce_paypal_wps_paypal_ipn_process()
 */
function commerce_marketplace_paypal_wps_adaptive_paypal_ipn_process($order, $payment_method, &$ipn) {
  foreach ($ipn['transaction'] as $transaction) {
    list($order_id, $timestamp) = explode('-', $transaction['invoice']);
    if ($order_id == $order->order_id) {
      $ipn_transaction = $transaction;
    }
  }

  // Do not perform any processing on WPS transactions here that do not have
  // transaction IDs, indicating they are non-payment IPNs such as those used
  // for subscription signup requests.
  // @TODO: Do we actually really need it for Adaptive Payments?
  if (empty($ipn_transaction['id_for_sender_txn'])) {
    return FALSE;
  }

  // Exit when we don't get a payment status we recognize.
  if (
    empty($ipn_transaction)
    || !in_array($ipn_transaction['status'], array('Failed', 'Voided', 'Pending', 'Completed', 'Refunded'), TRUE)
  ) {
    commerce_payment_redirect_pane_previous_page($order);
    return FALSE;
  }

  // If this is a prior authorization capture IPN for which we've already
  // created a transaction...
  if (
    in_array($ipn_transaction['status'], array('Voided', 'Completed'), TRUE)
    && !empty($ipn['auth_id'])
    && $auth_ipn = commerce_paypal_ipn_load($ipn['auth_id'])
  ) {
    // Load the prior IPN's transaction and update that with the capture values.
    // Marketplace-specific: we might have multiple payment transactions for
    // one IPN, so to load the correct transaction we first need to get its ID
    // from commerce_marketplace_paypal_ipn_transaction table based on order ID.
    $conditions = array(
      'ipn_id' => $auth_ipn['ipn_id'],
      'order_id' => $order->order_id,
    );
    $relationship = commerce_marketplace_paypal_ipn_transaction_load($conditions);
    $transaction = commerce_payment_transaction_load($relationship['transaction_id']);
  }
  else {
    // Create a new payment transaction for the order.
    $transaction = commerce_payment_transaction_new('paypal_wps', $order->order_id);
    $transaction->instance_id = $payment_method['instance_id'];
  }

  $transaction->remote_id = $ipn_transaction['id_for_sender_txn'];
  // Amounts are sent back as 'EUR 149.99'.
  list($currency, $amount) = explode(' ', $ipn_transaction['amount']);
  $transaction->amount = commerce_currency_decimal_to_amount($amount, $currency);
  $transaction->currency_code = $currency;
  $transaction->payload[REQUEST_TIME . '-ipn'] = $ipn;

  // Marketplace-specific: if we are processing multiple orders from one order
  // group, the amount returned by an IPN will be equal to sum of total amount
  // of all those orders. But the local payment transaction amounts should match
  // each order total, so it needs to be updated here.
  $order_wrapper = entity_metadata_wrapper('commerce_order', $order);
  if ($transaction->amount > $order_wrapper->commerce_order_total->amount->value()) {
    $transaction->amount = $order_wrapper->commerce_order_total->amount->value();
  }

  // Set the transaction's statuses based on the IPN's payment_status.
  $transaction->remote_status = $ipn_transaction['status'];

  // If we didn't get an approval response code...
  switch ($ipn_transaction['status']) {
    case 'Failed':
      $transaction->status = COMMERCE_PAYMENT_STATUS_FAILURE;
      $transaction->message = t("The payment has failed. This happens only if the payment was made from your customer’s bank account.");
      break;

    case 'Voided':
      $transaction->status = COMMERCE_PAYMENT_STATUS_FAILURE;
      $transaction->message = t('The authorization was voided.');
      break;

    case 'Pending':
      $transaction->status = COMMERCE_PAYMENT_STATUS_PENDING;
      $transaction->message = commerce_paypal_ipn_pending_reason($ipn['pending_reason']);
      break;

    case 'Completed':
      $transaction->status = COMMERCE_PAYMENT_STATUS_SUCCESS;
      $transaction->message = t('The payment has completed.');
      break;

    case 'Refunded':
      $transaction->status = COMMERCE_PAYMENT_STATUS_SUCCESS;
      $transaction->message = t('Refund for transaction @txn_id', array('@txn_id' => $ipn['parent_txn_id']));
      break;
  }

  // Save the transaction information.
  commerce_payment_transaction_save($transaction);
  $ipn['transaction_id'] = $transaction->transaction_id;

  // Marketplace-specific: we also need a connection between *all* created
  // payment transactions and the IPN, which cannot be saved in the main
  // commerce_paypal_ipn table, where 'transaction_id' column is defined as int
  // (see commerce_paypal_schema()) - therefore we need to save it
  // in a different table, allowing for one-to-many relationships between
  // IPN ID and transaction ID.
  // See commerce_marketplace_paypal_ipn_transaction_save().
  $ipn['transaction_ids'][$order->order_id] = $transaction->transaction_id;

  // Create a billing profile based on the IPN if enabled.
  // @TODO: Not tested yet.
//  if (!empty($payment_method['settings']['ipn_create_billing_profile']) && isset($order->commerce_customer_billing)) {
//
//    // If this order does not have a billing profile yet...
//    if ($order_wrapper->commerce_customer_billing->value() === NULL) {
//      // Ensure we have the required data in the IPN.
//      if (empty($ipn['residence_country']) || empty($ipn['first_name']) || empty($ipn['last_name'])) {
//        $data = array_intersect_key($ipn, drupal_map_assoc(array('residence_country', 'first_name', 'last_name')));
//        watchdog('commerce_marketplace_paypal_wps', 'A billing profile for Order @order_number could not be created due to insufficient data in the IPN:!data', array('@order_number' => $order->order_number, '!data' => '<pre>'. check_plain(print_r($data, TRUE)) .'</pre>'), WATCHDOG_WARNING);
//      }
//      else {
//        // Create the new profile now.
//        $profile = commerce_customer_profile_new('billing', $order->uid);
//
//        // Add the address value.
//        $profile_wrapper = entity_metadata_wrapper('commerce_customer_profile', $profile);
//
//        $profile_wrapper->commerce_customer_address = array_merge(addressfield_default_values(), array(
//          'country' => $ipn['residence_country'],
//          'name_line' => $ipn['first_name'] . ' ' . $ipn['last_name'],
//          'first_name' => $ipn['first_name'],
//          'last_name' => $ipn['last_name'],
//        ));
//
//        // Save the profile, reference it from the order, and save the order.
//        $profile_wrapper->save();
//        $order_wrapper->commerce_customer_billing = $profile_wrapper;
//        $order_wrapper->save();
//
//        watchdog('commerce_marketplace_paypal_wps', 'Billing profile created for Order @order_number containing the first and last names and residence country of the customer based on IPN data.', array('@order_number' => $order->order_number));
//      }
//    }
//  }

  commerce_payment_redirect_pane_next_page($order);
  watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: IPN processed for order @order_number with ID @txn_id.', array(
    '@order_number' => $order->order_number,
    '@txn_id' => $ipn_transaction['id_for_sender_txn'],
  ), WATCHDOG_INFO);
}

/**
 * Saves an IPN with some meta data related to local processing.
 *
 * @param $ipn
 *   An IPN array with additional parameters for the order_id and Commerce
 *     Payment transaction_id associated with the IPN.
 *
 * @return
 *   The operation performed by drupal_write_record() on save; since the IPN is
 *     received by reference, it will also contain the serial numeric ipn_id
 *     used locally.
 *
 * @see commerce_marketplace_paypal_process_ipn()
 * @see commerce_paypal_ipn_save()
 */
function commerce_marketplace_paypal_wps_adaptive_ipn_save(&$ipn) {
  // PayPal Adaptive Payments IPNs have a different format than standard WPS
  // IPNs, so we need to do a bit of moving stuff around.
  // @TODO: this is completely not finished, still requires doing it properly
  // - rethinking and refactoring.
  $ipn['txn_id'] = $ipn['pay_key'];
  $ipn['txn_type'] = $ipn['transaction_type'];
  $ipn['payer_email'] = $ipn['sender_email'];

  if (!empty($ipn['ipn_id']) && commerce_paypal_ipn_load($ipn['pay_key'])) {
    $ipn['changed'] = REQUEST_TIME;

    return drupal_write_record('commerce_paypal_ipn', $ipn, 'ipn_id');
  }
  else {
    $ipn['created'] = REQUEST_TIME;
    $ipn['changed'] = REQUEST_TIME;

    return drupal_write_record('commerce_paypal_ipn', $ipn);
  }
}
