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
    'api_logging' => array(),
    'marketplace_adaptive' => array(
      'payment_type' => 'parallel',
      'application_id' => '',
      'api_username' => '',
      'api_password' => '',
      'api_signature' => '',
      'payment_type' => 'parallel',
      'fees_payer' => 'EACHRECEIVER',
      'reverse_all_parallel_payments_on_error' => 'true',
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
  if ($payment_method['settings']['marketplace_adaptive']['payment_type'] == 'chained') {
    $total = commerce_marketplace_order_calculate_total($group_orders);
    $receivers[] = array(
      'amount' => commerce_currency_amount_to_decimal($total['amount'], $total['currency_code']),
      'invoiceId' => commerce_marketplace_paypal_ipn_invoice($group_orders),
      'email' => $payment_method['settings']['business'],
      'primary' => 'true',
    );
  }

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
    if ($payment_method['settings']['marketplace_adaptive']['payment_type'] == 'chained') {
      $receiver['primary'] = 'false';
    }

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
    'feesPayer' => $payment_method['settings']['marketplace_adaptive']['fees_payer'],
    'reverseAllParallelPaymentsOnError' => $payment_method['settings']['marketplace_adaptive']['reverse_all_parallel_payments_on_error'],
  );

  // Allow other modules to alter the request.
  drupal_alter('commerce_marketplace_paypal_wps_adaptive_get_paykey', $pay_request, $order, $payment_method);

  // Log the request if logging is enabled.
  if (!empty($payment_method['settings']['api_logging']['request'])) {
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
  if (!empty($payment_method['settings']['api_logging']['response'])) {
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
  if (!empty($payment_method['settings']['api_logging']['response'])) {
    watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: pay key response: !response', array(
      '!response' => '<pre>' . var_export($result, TRUE) . '</pre>',
    ), WATCHDOG_DEBUG);
  }

  // If everything was fine, just return received pay key.
  if (strtolower($result->responseEnvelope->ack) == 'success' && !empty($result->payKey)) {
    // Before saving received pay key, make sure there are no old pay chains
    // for the current order still existing in the database.
    db_delete('commerce_marketplace_paypal_wps_adaptive_pay_chain')
      ->condition('order_group', $order->order_group)
      ->execute();

    // Store the new pay chain.
    $pay_chain = array(
      'order_group' => $order->order_group,
      'pay_key' => $result->payKey,
      'currency' => $form['currency_code']['#value'],
      'data' => $receivers,
    );
    drupal_write_record('commerce_marketplace_paypal_wps_adaptive_pay_chain', $pay_chain);

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
 *
 *
 * @param array $ipn
 *   An IPN to check the transaction type of.
 *
 * @return bool
 *   A boolean indicating whether it is Adaptive Payments IPN or not.
 */
function commerce_marketplace_paypal_wps_ipn_is_adaptive($ipn) {
  if (
    !empty($ipn['transaction_type'])
    && $ipn['transaction_type'] == 'Adaptive Payment PAY'
  ) {
    return TRUE;
  }
  return FALSE;
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
  foreach ($ipn['transaction'] as $delta => $transaction) {
    list($order_id, $timestamp) = explode('-', $transaction['invoice']);
    if ($order_id == $order->order_id) {
      $ipn_transaction_delta = $delta;
      $ipn_transaction = $transaction;
    }
  }

  // Do not perform any processing on WPS transactions here that do not have
  // transaction IDs, indicating they are non-payment IPNs such as those used
  // for subscription signup requests.
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
  // @TODO: this is not tested for Adaptive Payments at all yet.
  // Probably doesn't work then, and actually awaits adding authorisation
  // support to this module.
  if (
    in_array($ipn_transaction['status'], array('Voided', 'Completed'), TRUE)
    && !empty($ipn['auth_id'])
    && $auth_ipn = commerce_marketplace_paypal_ipn_load($ipn['auth_id'], $order->order_id)
  ) {
    // Load the prior IPN's transaction and update that with the capture values.
    // Marketplace-specific: we might have multiple payment transactions for
    // one IPN, so to load the correct transaction we first need to get its ID
    // from commerce_marketplace_paypal_wps_adaptive_ipn table based on order ID.
    $transaction = commerce_payment_transaction_load($auth_ipn['transaction_id']);
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
  // Add order_id and transaction_id to the IPN's transaction subarray so that
  // we can save it in commerce_marketplace_paypal_wps_adaptive_ipn table in
  // commerce_marketplace_paypal_wps_commerce_marketplace_paypal_ipn_save().
  $ipn['transaction'][$ipn_transaction_delta]['order_id'] = $order->order_id;
  $ipn['transaction'][$ipn_transaction_delta]['transaction_id'] = $transaction->transaction_id;

  commerce_payment_redirect_pane_next_page($order);
  watchdog('commerce_marketplace_paypal_wps', 'Adaptive Payments: IPN processed for order @order_number with ID @txn_id.', array(
    '@order_number' => $order->order_number,
    '@txn_id' => $ipn_transaction['id_for_sender_txn'],
  ), WATCHDOG_INFO);
}
